# Run from a VS developer shell. WARP; no NVIDIA GPU or game required.
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$build = Join-Path $env:TEMP ('nr-scene-input-' + [guid]::NewGuid())
New-Item -ItemType Directory -Path $build | Out-Null
& cl.exe /nologo /std:c++20 /EHsc /DNOMINMAX /UNDEBUG "/I$PSScriptRoot/hdr_stubs" "/I$repo/OptiScaler" "/Fo$build/" "/Fe$build/adapter.exe" "$PSScriptRoot/nr_scene_input.cpp" "$repo/OptiScaler/dlssnr/DlssNr_GpuLifetime.cpp" d3d12.lib dxgi.lib d3dcompiler.lib ole32.lib
if ($LASTEXITCODE -ne 0) { throw 'NR scene input compilation failed' }
& "$build/adapter.exe"
if ($LASTEXITCODE -ne 0) { throw 'NR scene input test failed' }
