"""Compile production HDR/NR domain decisions, including pending restart preferences."""
from pathlib import Path
import subprocess,tempfile
r=Path(__file__).resolve().parents[1]
s=(r/'OptiScaler/shaders/dlssnr/DlssNr_Dx12.cpp').read_text(encoding='utf-8')
d=(r/'OptiScaler/shaders/dlssnr/DlssNr_DeferredSr.inl').read_text(encoding='utf-8')
a=s.index('    const int inputDomain =');b=s.index('\n    // Logged whenever',a);reset=s[a:b]
a=d.index('    if (DlssSceneInput::bridgeSceneHdr)');b=d.index('    const bool splitWork',a);flags=d[a:b]
a=d.index('    const bool hdrCarrier=');b=d.index('\n    if(g.hdrCarrier',a);carrier=d[a:b]
code=r"""
#include <cassert>
#include <cstdlib>
#include <array>
#define LOG_INFO(...) ((void)0)
constexpr unsigned NVSDK_NGX_DLSS_Feature_Flags_IsHDR=1;
namespace DlssSceneInput {bool bridgeSceneHdr=false,bridgeHdrShoulder=false;}
namespace FfxivSceneHdr {bool available=false;struct Scene{bool hdr;};Scene Open(int,int,int,bool){return {available};}}
struct Generation{int device=0,w=1,h=1;};
struct Frame{bool ColourIsLinearHdr=false,ExposedSceneHdr=false,BeforeUpscale=true,RayReconstruction=false,Reset=false;};
struct NR{int inputDomain=-1;bool reset=false,heldActive=true;std::array<bool,4>passNeedsReset{};}g_nr;
bool Policy(bool active,bool available,bool privateJob,bool desired){
 DlssSceneInput::bridgeSceneHdr=active;FfxivSceneHdr::available=available;Generation g;unsigned flags=0;
"""+flags+carrier+r"""
 if(bool(flags&1)!=active)std::exit(1);return hdrCarrier;
}
void Apply(Frame frame,bool lightingReset=false){
"""+reset+r"""
}
void Cleared(){g_nr.reset=false;g_nr.passNeedsReset.fill(false);}
void ExpectReset(){assert(g_nr.reset);for(bool reset:g_nr.passNeedsReset)assert(reset);}
int main(){
 // Effective feature HDR and desired checkbox may disagree until restart/recreation.
 for(bool active:{false,true})for(bool desired:{false,true})for(bool available:{false,true})for(bool privateJob:{false,true}){
  assert(Policy(active,available,privateJob,desired)==(active&&available&&!privateJob));
 }
 for(bool active:{false,true}){g_nr.inputDomain=-1;DlssSceneInput::bridgeSceneHdr=active;
  // Ordinary SDR, scene HDR, SDR fallback, exposed HDR, pre/post SR, and SR/RR transitions.
  std::array<Frame,7> frames={Frame{},Frame{true,true},Frame{},Frame{true,false},Frame{false,false,false},Frame{false,false,false,true},Frame{}};
  for(auto frame:frames){Cleared();Apply(frame);ExpectReset();assert(!g_nr.heldActive);Cleared();Apply(frame);assert(!g_nr.reset);for(bool reset:g_nr.passNeedsReset)assert(!reset);}
 }
 Frame post{false,false,false};DlssSceneInput::bridgeSceneHdr=false;Apply(post);Cleared();DlssSceneInput::bridgeSceneHdr=true;Apply(post);ExpectReset();
 Cleared();Apply(post);assert(!g_nr.reset);DlssSceneInput::bridgeHdrShoulder=true;Apply(post);ExpectReset();
 Cleared();Apply(post);assert(!g_nr.reset);post.Reset=true;Apply(post);ExpectReset();
 Cleared();post.Reset=false;Apply(post,true);ExpectReset();
}
"""
with tempfile.TemporaryDirectory(prefix='hdr-nr-policy-',dir=r/'tests') as t:
 b=Path(t);(b/'test.cpp').write_text(code,encoding='utf-8')
 subprocess.run(['cl','/nologo','/std:c++20','/EHsc','/UNDEBUG',str(b/'test.cpp'),f'/Fo{b}/',f'/Fe{b}/test.exe'],check=True)
 subprocess.run([str(b/'test.exe')],check=True)
 legacy=code.replace('if (DlssSceneInput::bridgeSceneHdr)\n        flags', 'if (desired)\n        flags')
 assert legacy!=code
 (b/'test.cpp').write_text(legacy,encoding='utf-8')
 subprocess.run(['cl','/nologo','/std:c++20','/EHsc','/UNDEBUG',str(b/'test.cpp'),f'/Fo{b}/',f'/Fe{b}/legacy.exe'],check=True)
 assert subprocess.run([str(b/'legacy.exe')]).returncode==1, 'negative control did not detect pending HDR preference changing the private domain'
 print('PASS: negative control reproduces desired-versus-active HDR mode mismatch')
print('PASS: effective HDR feature controls PreSR domain/carrier; NR colour, placement, SR/RR, game and lighting transitions reset every pass without resetting steady state')
