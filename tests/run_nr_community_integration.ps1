# VS developer shell; WARP, no game or NVIDIA NR runtime needed.
$ErrorActionPreference='Stop'
$repo=(Resolve-Path "$PSScriptRoot/..").Path
$build=Join-Path ([IO.Path]::GetTempPath()) ('nr-integration-'+[guid]::NewGuid())
New-Item -ItemType Directory -Path $build | Out-Null
& cl.exe /nologo /std:c++20 /EHsc /UNDEBUG "/Fo$build/guides.obj" "/Fe$build/guides.exe" "$PSScriptRoot/nr_guides_smoke.cpp"
if($LASTEXITCODE) { throw 'Guide unit compilation failed' }
& "$build/guides.exe"
if($LASTEXITCODE) { throw 'Guide metadata test failed' }
& cl.exe /nologo /std:c++20 /EHsc /UNDEBUG "/Fo$build/shader.obj" "/Fe$build/shader.exe" "$PSScriptRoot/nr_community_shader.cpp" d3d11.lib d3dcompiler.lib
if($LASTEXITCODE) { throw 'Shader compilation failed' }
$shaders="$repo/OptiScaler/shaders/dlssnr/precompile"
& "$build/shader.exe" "$shaders/dlssnr.hlsl" "$shaders/dlssnr_spatial.hlsl" "$shaders/dlssnr_guide_match.hlsl"
if($LASTEXITCODE) { throw 'Shader regression failed' }
