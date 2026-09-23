# Run in a Visual Studio developer shell; no NVIDIA runtime or running game.
$ErrorActionPreference='Stop'
$taskRepo=(Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$taskBuild=Join-Path ([IO.Path]::GetTempPath()) ('presr-bounds-'+[guid]::NewGuid())
New-Item -ItemType Directory -Path $taskBuild | Out-Null
$taskSource=[IO.File]::ReadAllText((Join-Path $taskRepo 'OptiScaler/shaders/dlssnr/DlssNr_DeferredSr.inl'))
$taskStart=$taskSource.IndexOf('bool BoundResidual(')
$taskEnd=$taskSource.IndexOf('// Background GPU job:', $taskStart)
if($taskStart -lt 0 -or $taskEnd -lt 0) { throw 'Bounds production function changed.' }
[IO.File]::WriteAllText((Join-Path $taskBuild 'presr-bounds-production.inl'),$taskSource.Substring($taskStart,$taskEnd-$taskStart))
& cl.exe /nologo /std:c++20 /EHsc /UNDEBUG "/I$taskBuild" "/Fo$taskBuild/lifecycle.obj" "/Fe$taskBuild/lifecycle.exe" "$PSScriptRoot/nr_presr_bounds_lifecycle.cpp"
if($LASTEXITCODE -ne 0) { throw 'Bounds lifecycle compilation failed.' }
& "$taskBuild/lifecycle.exe"
if($LASTEXITCODE -ne 0) { throw 'Bounds lifecycle regression failed.' }
foreach($taskName in @('nr_presr_bounds_shader','nr_skin_shader_smoke')) {
    & cl.exe /nologo /std:c++20 /EHsc /UNDEBUG "/Fo$taskBuild/$taskName.obj" "/Fe$taskBuild/$taskName.exe" "$PSScriptRoot/$taskName.cpp" d3d11.lib d3dcompiler.lib
    if($LASTEXITCODE -ne 0) { throw "Shader test compilation failed: $taskName" }
    & "$taskBuild/$taskName.exe" "$taskRepo/OptiScaler/shaders/dlssnr/precompile/dlssnr.hlsl"
    if($LASTEXITCODE -ne 0) { throw "Shader regression failed: $taskName" }
}
