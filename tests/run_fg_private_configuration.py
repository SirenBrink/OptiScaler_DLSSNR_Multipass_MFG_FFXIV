from pathlib import Path
import argparse, subprocess, tempfile
p=argparse.ArgumentParser(description="Test private FG configuration using supported local NVIDIA DLLs; does not test rendering")
p.add_argument("runtime",type=Path,nargs="+")
a=p.parse_args();r=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="fg-native-config-",dir=r/"tests") as t:
 b=Path(t)
 (b/"pch.h").write_text("#include <windows.h>\n#include <filesystem>\n#include <vector>\n#include <string>\n#include <atomic>\n#include <cstdio>\n#define LOG_INFO(...) ((void)0)\n#define LOG_WARN(...) ((void)0)\n")
 (b/"Config.h").write_text("#pragma once\nstruct Config {struct Int{int value_or_default()const{return 2;}}FGDLSSGRenderPreset;static Config*Instance(){static Config c;return &c;}};\n")
 exe=b/"probe.exe"
 subprocess.run(["cl","/nologo","/std:c++20","/EHsc","/UNDEBUG",f"/I{b}",f"/I{r/'OptiScaler/include'}",f"/I{r/'OptiScaler'}",str(r/"tests/fg_private_configuration.cpp"),f"/Fo{b}/",f"/Fe{exe}","bcrypt.lib",str(r/"OptiScaler/library/detours/detours.lib")],check=True)
 for runtime in a.runtime:
  subprocess.run([str(exe),str(runtime.resolve())],check=True)
  subprocess.run([str(exe),str(runtime.resolve()),"mutated-in-process"],check=True)
