# Run in a Visual Studio developer shell. Uses D3D11 WARP, not a running game or NVIDIA runtime.
$ErrorActionPreference='Stop'
$repo=(Resolve-Path "$PSScriptRoot/..").Path
$build=Join-Path ([IO.Path]::GetTempPath()) ('nr-spatial-'+[guid]::NewGuid())
New-Item -ItemType Directory -Path $build | Out-Null
foreach($name in @('nr_spatial_mapping_smoke','nr_spatial_warp_smoke')) {
    $libs=@(); if($name -eq 'nr_spatial_warp_smoke') {$libs=@('d3d11.lib','d3dcompiler.lib')}
    & cl.exe /nologo /std:c++20 /EHsc /UNDEBUG "/Fo$build/$name.obj" "/Fe$build/$name.exe" "$PSScriptRoot/$name.cpp" @libs
    if($LASTEXITCODE) {throw "Compilation failed: $name"}
    & "$build/$name.exe" "$repo/OptiScaler/shaders/dlssnr/precompile/dlssnr_spatial.hlsl"
    if($LASTEXITCODE) {throw "Test failed: $name"}
}
