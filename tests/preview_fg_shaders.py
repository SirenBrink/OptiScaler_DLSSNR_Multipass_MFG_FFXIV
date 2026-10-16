"""Compile the runtime-only preview map and HUD-paste shaders with FXC."""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
sdk_bin = Path(r"C:\Program Files (x86)\Windows Kits\10\bin")
fxc = next(iter(sorted(sdk_bin.glob("*/x64/fxc.exe"), reverse=True)), None)
assert fxc is not None, "Windows SDK FXC compiler is required"
sources = [
    (root / "OptiScaler/misc/FfxivPreviewFGMap.cpp", "shader", "Main", "ps_5_0"),
    (root / "OptiScaler/misc/FfxivPreviewFlow.h", "FlowShaderCode", "CSMain", "cs_5_0"),
    (root / "OptiScaler/misc/UiPaste.cpp", "ProduceShaderCode", "CSMain", "cs_5_0"),
    (root / "OptiScaler/misc/UiPaste.cpp", "PasteShaderCode", "VSMain", "vs_5_0"),
    (root / "OptiScaler/misc/UiPaste.cpp", "PasteShaderCode", "PSMain", "ps_5_0"),
]
with tempfile.TemporaryDirectory(dir=root / "tests") as folder:
    for index, (path, name, entry, target) in enumerate(sources):
        text = path.read_text(encoding="utf-8")
        match = re.search(rf"(?:constexpr char|const char\*)\s+{name}\[?\]?\s*=\s*R\"\((.*?)\)\";", text, re.S)
        assert match, (path, name)
        source = Path(folder) / f"shader-{index}.hlsl"
        output = Path(folder) / f"shader-{index}.cso"
        source.write_text(match.group(1), encoding="utf-8")
        subprocess.run([fxc, "/nologo", "/T", target, "/E", entry, "/Fo", output, source], check=True)
print("Preview FG shaders compile")
