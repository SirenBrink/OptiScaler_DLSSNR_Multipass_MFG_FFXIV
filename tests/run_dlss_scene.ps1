$ErrorActionPreference='Stop'
$repo=(Resolve-Path "$PSScriptRoot/..").Path
$build=Join-Path $env:TEMP ('optihdr-dlss-' + [guid]::NewGuid())
New-Item -ItemType Directory -Force $build | Out-Null
& cl.exe /nologo /std:c++20 /EHsc /DNOMINMAX /UNDEBUG "/I$PSScriptRoot/hdr_stubs" "/I$repo/OptiScaler" "/I$repo/external/nvngx_dlss_sdk" "/Fo$build/" "/Fe$build/test.exe" "$PSScriptRoot/dlss_scene_dx12.cpp" "$repo/OptiScaler/shaders/hdr/Hdr10.cpp" "$repo/OptiScaler/dlssnr/DlssNr_GpuLifetime.cpp" d3d12.lib dxgi.lib d3dcompiler.lib ole32.lib user32.lib
if($LASTEXITCODE -ne 0){throw 'Compilation failed'}
& "$build/test.exe"
if($LASTEXITCODE -ne 0){throw 'GPU test failed'}
