from pathlib import Path
import subprocess, tempfile
r=Path(__file__).resolve().parents[1]
s=(r/'OptiScaler/hooks/D3D11_Hooks.cpp').read_text(encoding='utf-8-sig')
start=s.index('namespace {\nstd::array<Dx11AutomaticMipBias::Set')
end=s.index('\nstatic void HookToDeviceLocal',start)
with tempfile.TemporaryDirectory(prefix='auto-mip-',dir=r/'tests') as temp:
 b=Path(temp)
 (b/'automatic_mip_hooks.inl').write_text(s[start:end])
 lib=r/'OptiScaler/library/detours/detours.lib'
 subprocess.run(['cl','/nologo','/std:c++20','/EHsc','/UNDEBUG','/DTEST_PRODUCTION_HOOKS',f'/I{b}',f'/I{r/"OptiScaler"}',f'/I{r/"OptiScaler/include"}',str(r/'tests/automatic_mip_bias.cpp'),f'/Fo{b}/',f'/Fe{b}/test.exe','d3d11.lib',str(lib)],check=True)
 subprocess.run([str(b/'test.exe')],check=True)
