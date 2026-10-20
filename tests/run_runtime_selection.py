from pathlib import Path
import tempfile,subprocess
r=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='runtime-selection-',dir=r/'tests') as t:
 b=Path(t);assert b.resolve().is_relative_to((r/'tests').resolve())
 (b/'Config.h').write_text("""#pragma once
#include <filesystem>
#include <string>
template<class T>struct Value{T data{};Value& operator=(T v){data=v;return *this;}bool has_value()const{return !data.empty();}T value_or_default()const{return data;}T value()const{return data;}};
struct Config{Value<std::filesystem::path> MainDllPath;Value<std::string> DlssNrRuntime;struct Int{int data=0;Int& operator=(int v){data=v;return *this;}int value_or_default()const{return data;}}FGDLSSGRenderPreset;static Config* Instance(){static Config c;return &c;}};
""")
 (b/'Util.h').write_text("""#pragma once
#include <filesystem>
namespace Util { inline std::filesystem::path base;inline auto DllPath(){return base/"winmm.dll";}inline auto ExePath(){return base/"ffxiv_dx11.exe";} }
""")
 (b/'Logger.h').write_text('''#pragma once
#define LOG_INFO(...) ((void)0)
#define LOG_WARN(...) ((void)0)
''')
 source=(r/'OptiScaler/nvapi/NvApiHooks.cpp').read_text(encoding='utf-8-sig')
 start=source.index('        const auto fgPreset=(settingId==NGX_DLSS_FG_OVERRIDE_ID')
 end=source.index('        constexpr NvU32 streamlineOverrideId',start)
 (b/'preset_override.inl').write_text(source[start:end])
 subprocess.run(['cl','/nologo','/std:c++20','/EHsc','/UNDEBUG',f'/I{b}',f'/I{r/"OptiScaler"}',str(r/'tests/runtime_selection.cpp'),f'/Fo{b}/',f'/Fe{b}/test.exe'],check=True)
 for mode in ['explicit','missing','legacy','preset-a','preset-b']:
  subprocess.run([str(b/'test.exe'),str(b/mode),mode],check=True)
