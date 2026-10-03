param([string]$XessInclude = (Join-Path $PSScriptRoot "../external/xess/inc/xess_fg"))
# Run from a Visual Studio developer shell. WARP; no HDR display required.
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$build = Join-Path $env:TEMP ('optihdr-' + [guid]::NewGuid())
New-Item -ItemType Directory -Path $build | Out-Null
& cl.exe /nologo /std:c++20 /EHsc /DNOMINMAX /UNDEBUG "/Fo$build/" "/Fe$build/shader.exe" "$PSScriptRoot/hdr10_shader.cpp"
if ($LASTEXITCODE -ne 0) { throw 'HDR shader test compilation failed' }
& "$build/shader.exe"
if ($LASTEXITCODE -ne 0) { throw 'HDR shader test failed' }
& cl.exe /nologo /std:c++20 /EHsc /DNOMINMAX /UNDEBUG "/I$PSScriptRoot/hdr_stubs" "/I$repo/OptiScaler" "/I$XessInclude" "/Fo$build/" "/Fe$build/dx12.exe" "$PSScriptRoot/hdr10_dx12.cpp" "$repo/OptiScaler/shaders/hdr/Hdr10.cpp" "$repo/OptiScaler/dlssnr/DlssNr_GpuLifetime.cpp" d3d12.lib dxgi.lib d3dcompiler.lib ole32.lib user32.lib
if ($LASTEXITCODE -ne 0) { throw 'HDR DX12 test compilation failed' }
& "$build/dx12.exe"
if ($LASTEXITCODE -ne 0) { throw 'HDR DX12 test failed' }

& "$build/dx12.exe" --untracked
if ($LASTEXITCODE -ne 0) { throw "HDR missing-notification negative control failed" }

& "$build/dx12.exe" --xefg
if ($LASTEXITCODE -ne 0) { throw "XeFG HDR tag/copy lifetime test failed" }

& "$build/dx12.exe" --scene
if ($LASTEXITCODE -ne 0) { throw "Scene HDR bridge/lifetime test failed" }
