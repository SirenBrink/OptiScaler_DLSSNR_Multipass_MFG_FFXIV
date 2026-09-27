# Run from a Visual Studio developer shell. Uses WARP, no NVIDIA runtime or game.
$ErrorActionPreference = 'Stop'
$taskRepo = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$taskBuild = Join-Path ([IO.Path]::GetTempPath()) ('nr-community-' + [guid]::NewGuid())
New-Item -ItemType Directory -Path $taskBuild | Out-Null
foreach ($taskName in @('nr_model_size', 'nr_vit_reuse_smoke', 'nr_multipass_feedback_smoke')) {
    $taskLibraries = @()
    if ($taskName -eq 'nr_multipass_feedback_smoke') { $taskLibraries = @('d3d11.lib', 'd3dcompiler.lib') }
    & cl.exe /nologo /std:c++20 /EHsc /UNDEBUG "/Fo$taskBuild/$taskName.obj" "/Fe$taskBuild/$taskName.exe" "$PSScriptRoot/$taskName.cpp" @taskLibraries
    if ($LASTEXITCODE -ne 0) { throw "Compilation failed: $taskName" }
    & "$taskBuild/$taskName.exe" "$taskRepo/OptiScaler/shaders/dlssnr/precompile/dlssnr.hlsl"
    if ($LASTEXITCODE -ne 0) { throw "Test failed: $taskName" }
}
