$ErrorActionPreference='Stop'
$taskRepo=Split-Path $PSScriptRoot
$taskBuild=Join-Path $env:TEMP ('dynamic-mfg-'+[guid]::NewGuid())
New-Item -ItemType Directory -Path $taskBuild | Out-Null
$taskSource=[IO.File]::ReadAllText("$taskRepo/OptiScaler/hooks/Reflex_Hooks.cpp")
$taskStart=$taskSource.IndexOf('void ReflexHooks::update(bool fgActive, bool isVulkan)')
$taskEnd=$taskSource.IndexOf('// 0 - disables the fps cap',$taskStart)
if($taskStart -lt 0 -or $taskEnd -lt 0){throw 'Production Reflex update not found'}
$taskUpdate=$taskSource.Substring($taskStart,$taskEnd-$taskStart)
[IO.File]::WriteAllText("$taskBuild/dynamic_mfg_update_production.inl",$taskUpdate)
& cl /nologo /std:c++20 /EHsc /W4 "/I$taskBuild" "/Fo$taskBuild/test.obj" "/Fe$taskBuild/test.exe" "$PSScriptRoot/dynamic_mfg_pacing.cpp"
if($LASTEXITCODE){throw 'Dynamic pacing test compilation failed'}
& "$taskBuild/test.exe"
if($LASTEXITCODE){throw 'Dynamic pacing regression failed'}
# Demonstrate that the fixture catches the previous factor-driven limiter path.
$taskBad=$taskUpdate.Replace('if (nativePacing)','if (false && nativePacing)')
[IO.File]::WriteAllText("$taskBuild/dynamic_mfg_update_production.inl",$taskBad)
& cl /nologo /std:c++20 /EHsc "/I$taskBuild" "/Fo$taskBuild/old.obj" "/Fe$taskBuild/old.exe" "$PSScriptRoot/dynamic_mfg_pacing.cpp"
if($LASTEXITCODE){throw 'Negative-control compilation failed'}
& "$taskBuild/old.exe"
if($LASTEXITCODE -ne 1){throw 'Old limiter conflict was not detected'}
Write-Output 'PASS: negative control catches the old Dynamic MFG limiter conflict'
