from pathlib import Path
import tempfile,subprocess
r=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='fg-cache-',dir=r/'tests') as t:
 b=Path(t)
 (b/'Config.h').write_text('#pragma once\nstruct Config {struct Int{int value_or_default()const{return 2;}}FGDLSSGRenderPreset;static Config*Instance(){static Config c;return &c;}};\n')
 subprocess.run(['cl','/nologo','/std:c++20','/EHsc','/UNDEBUG',f'/I{b}',str(r/'tests/cached_fg_preset.cpp'),f'/Fo{b}/',f'/Fe{b}/test.exe'],check=True)
 subprocess.run([str(b/'test.exe')],check=True)
