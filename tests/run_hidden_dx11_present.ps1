$ErrorActionPreference='Stop'
$taskRepo=Split-Path $PSScriptRoot
$taskBuild=Join-Path $env:TEMP ('hidden-present-'+[guid]::NewGuid())
New-Item -ItemType Directory -Path $taskBuild | Out-Null
$taskSource=[IO.File]::ReadAllText("$taskRepo/OptiScaler/with_dx12/dx11_with_dx12_sc.cpp")
$taskStart=$taskSource.IndexOf('FramePacingTrace::Scope hiddenTiming')
$taskEnd=$taskSource.IndexOf("`n    }",$taskStart)
if($taskStart -lt 0 -or $taskEnd -lt 0){throw 'Production hidden-present path not found'}
$taskBody=$taskSource.Substring($taskStart,$taskEnd-$taskStart)
[IO.File]::WriteAllText("$taskBuild/hidden_present_production.inl",$taskBody)
& cl /nologo /std:c++20 /EHsc /W4 "/I$taskBuild" "/Fo$taskBuild/test.obj" "/Fe$taskBuild/test.exe" "$PSScriptRoot/hidden_dx11_present.cpp"
if($LASTEXITCODE){throw 'Hidden-present test compilation failed'}
& "$taskBuild/test.exe"
if($LASTEXITCODE){throw 'Hidden-present regression failed'}
$taskBad=$taskBody.Replace('Flags | DXGI_PRESENT_DO_NOT_WAIT','Flags')
[IO.File]::WriteAllText("$taskBuild/hidden_present_production.inl",$taskBad)
& cl /nologo /std:c++20 /EHsc "/I$taskBuild" "/Fo$taskBuild/old.obj" "/Fe$taskBuild/old.exe" "$PSScriptRoot/hidden_dx11_present.cpp"
if($LASTEXITCODE){throw 'Negative-control compilation failed'}
& "$taskBuild/old.exe"
if($LASTEXITCODE -ne 1){throw 'Old blocking present was not detected'}
Write-Output 'PASS: negative control catches the previous blocking hidden-present request'
