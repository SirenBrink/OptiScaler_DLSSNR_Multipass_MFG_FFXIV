param([string]$SimpleIni = (Join-Path $PSScriptRoot '../external/simpleini'))
# Run from a VS developer shell. WARP; no game or HDR display required.
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$build = Join-Path $env:TEMP ('scene-hdr-' + [guid]::NewGuid())
New-Item -ItemType Directory -Path $build | Out-Null
& cl.exe /nologo /std:c++20 /EHsc /DNOMINMAX /UNDEBUG "/I$PSScriptRoot/hdr_stubs" "/I$repo/OptiScaler" "/Fo$build/" "/Fe$build/capture.exe" "$PSScriptRoot/scene_hdr_capture.cpp"
if ($LASTEXITCODE -ne 0) { throw 'Scene capture test compilation failed' }
& "$build/capture.exe"
if ($LASTEXITCODE -ne 0) { throw 'Scene capture test failed' }
& python "$PSScriptRoot/run_config_persistence.py" $SimpleIni
if ($LASTEXITCODE -ne 0) { throw 'HDR mode persistence test failed' }
