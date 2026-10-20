from pathlib import Path
import tempfile,subprocess
r=Path(__file__).resolve().parents[1]
def function(s,signature):
 start=s.index(signature);a=s.index('{',start);i=a+1;depth=1
 while depth:
  depth+=(s[i]=='{')-(s[i]=='}');i+=1
 return s[start:i]
with tempfile.TemporaryDirectory(prefix='bias-jitter-',dir=r/'tests') as t:
 b=Path(t);assert b.resolve().is_relative_to((r/'tests').resolve())
 (b/'Config.h').write_text("""#pragma once
#include <optional>
template<class T>struct Value:std::optional<T>{using std::optional<T>::operator=;T value_or_default()const{return this->value_or(T{});}};
struct Config{Value<float>MipmapBiasOverride;Value<unsigned>AnisotropyOverride;Value<bool>MipmapBiasFixedOverride,MipmapBiasScaleOverride,MipmapBiasOverrideAll,ExtendedStateRestore,FfxivAutomaticMipBias;static Config* Instance(){static Config c;return &c;}};
""")
 (b/'State.h').write_text('''#pragma once
struct State{float lastMipBias=100,lastMipBiasMax=-100;static State& Instance(){static State s;return s;}};
''')
 (b/'Logger.h').write_text('''#pragma once
#define LOG_DEBUG(...) ((void)0)
#define LOG_ERROR(...) ((void)0)
#define LOG_FUNC(...) ((void)0)
''')
 (b/'nvsdk_ngx.h').write_text('''#pragma once
#include <cstring>
constexpr int NVSDK_NGX_Result_Success=1;
constexpr auto NVSDK_NGX_Parameter_Jitter_Offset_X="X";
constexpr auto NVSDK_NGX_Parameter_Jitter_Offset_Y="Y";
struct NVSDK_NGX_Parameter{float x=0,y=0;bool haveX=true,haveY=true;unsigned writes=0;int Get(const char* key,float* v){bool xx=std::strcmp(key,"X")==0;if(!(xx?haveX:haveY))return 0;*v=xx?x:y;return 1;}void Set(const char* key,float v){++writes;(std::strcmp(key,"X")==0?x:y)=v;}};
''')
 s=(r/'OptiScaler/hooks/D3D12_Hooks.cpp').read_text(encoding='utf-8-sig')
 signatures=['static void ApplySamplerOverrides(D3D12_STATIC_SAMPLER_DESC&','static void ApplySamplerOverrides(D3D12_STATIC_SAMPLER_DESC1&','static HRESULT hkD3D12SerializeRootSignature(','static HRESULT hkD3D12SerializeVersionedRootSignature(','static HRESULT hkCreateRootSignature(','static void hkCreateSampler(']
 bodies=[function(s,x)for x in signatures]
 s=(r/'OptiScaler/hooks/D3D11_Hooks.cpp').read_text(encoding='utf-8-sig');start=s.index('VALIDATE_HOOK(hkCreateSamplerState,');bodies+=[function(s[start:],'static HRESULT hkCreateSamplerState(')]
 (b/'sampler_production.inl').write_text('\n'.join(bodies))
 cmd=['cl','/nologo','/std:c++20','/EHsc','/UNDEBUG',f'/I{b}',f'/I{r/"OptiScaler"}',str(r/'tests/bias_jitter.cpp'),f'/Fo{b}/',f'/Fe{b}/test.exe','d3d12.lib','dxgi.lib','dxguid.lib']
 subprocess.run(cmd,check=True);subprocess.run([str(b/'test.exe')],check=True)
