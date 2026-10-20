"""Exercise the production NR runtime transition with pending feature ownership."""
from pathlib import Path
import tempfile,subprocess
r=Path(__file__).resolve().parents[1]
s=(r/'OptiScaler/shaders/dlssnr/DlssNr_Dx12.cpp').read_text()
a=s.index('    const auto runtime=DlssNr::RuntimeSelection::D3D12();',s.index('void DlssNr_Dx12::Dispatch('));b=s.index('\n\n    if (g_nr.failed || cmdList',a);body=s[a:b]
code=r'''
#include <array>
#include <cassert>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#define LOG_INFO(...) ((void)0)
namespace DlssNr {constexpr unsigned MaxPassCount=4;namespace RuntimeSelection {
struct Selection{uint64_t revision=1;};inline auto selected=std::make_shared<Selection>();
auto D3D12(){return selected;}
}}
struct State{
 uint64_t runtimeRevision=0;void* feature=nullptr;
 bool featurePendingSubmission=true;std::function<bool()> featureReady=[] {return false;};
 void* passFeature[4]{};bool passNeedsReset[4]{},passCreateFailed[4]{},passPendingSubmission[4]{};
 std::function<bool()> passReady[4];bool failed=true,reset=false;const char* reason="failed";std::string modelError="failed";
}g_nr;
std::vector<void*> retired;int forgotten=0;
void ParkNrFeature(void*& f){if(f){retired.push_back(f);f=nullptr;}}
void ForgetCalibration(){++forgotten;}
void Apply(){
''' + body + r'''
}
int main(){
 g_nr.feature=(void*)1;for(unsigned i=1;i<4;++i){g_nr.passFeature[i]=(void*)(uintptr_t(i+1));g_nr.passCreateFailed[i]=g_nr.passPendingSubmission[i]=true;g_nr.passReady[i]=[]{return false;};}
 Apply();assert(g_nr.runtimeRevision==1&&!g_nr.feature&&!g_nr.failed&&g_nr.reset&&retired.size()==4&&forgotten==1);
 for(unsigned i=1;i<4;++i)assert(!g_nr.passFeature[i]&&g_nr.passNeedsReset[i]&&!g_nr.passCreateFailed[i]&&!g_nr.passPendingSubmission[i]&&!g_nr.passReady[i]);
 assert(!g_nr.featurePendingSubmission&&!g_nr.featureReady&&g_nr.modelError.empty());
 g_nr.feature=(void*)9;g_nr.failed=true;Apply();assert(g_nr.feature==(void*)9&&g_nr.failed&&retired.size()==4); // unchanged does not rebuild/retry
 DlssNr::RuntimeSelection::selected->revision=2;Apply();assert(!g_nr.feature&&!g_nr.failed&&retired.back()==(void*)9&&retired.size()==5);
 Apply();assert(retired.size()==5); // old GPU ownership was parked once, never freed by the transition
}
'''
with tempfile.TemporaryDirectory(prefix='nr-live-transition-',dir=r/'tests') as t:
 b=Path(t);(b/'test.cpp').write_text(code)
 subprocess.run(['cl','/nologo','/std:c++20','/EHsc','/UNDEBUG',str(b/'test.cpp'),f'/Fo{b}/',f'/Fe{b}/test.exe'],check=True)
 subprocess.run([str(b/'test.exe')],check=True)
print('PASS: production NR transition retires scene/multipass features once, retains pending GPU ownership, resets history and recovers failures only on a new runtime request')
