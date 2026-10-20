$ErrorActionPreference='Stop'
$taskRepo=Split-Path $PSScriptRoot
$taskBuild=Join-Path $env:TEMP ('auxiliary-present-'+[guid]::NewGuid())
New-Item -ItemType Directory -Path $taskBuild | Out-Null
$taskSource=[IO.File]::ReadAllText("$taskRepo/OptiScaler/wrapped/wrapped_swapchain.cpp")
$taskStart=$taskSource.IndexOf('if (InteropAuxiliaryPresent::Active())')
if($taskStart -lt 0){throw 'Production auxiliary bypass not found'}
$taskOpen=$taskSource.IndexOf('{',$taskStart)
$taskEnd=$taskOpen+1;$taskDepth=1
while($taskDepth -gt 0){if($taskSource[$taskEnd] -eq '{'){$taskDepth++};if($taskSource[$taskEnd] -eq '}'){$taskDepth--};$taskEnd++}
$taskBody=$taskSource.Substring($taskStart,$taskEnd-$taskStart)
[IO.File]::WriteAllText("$taskBuild/auxiliary_present_production.inl",$taskBody)
& cl /nologo /std:c++20 /EHsc /W4 "/I$taskBuild" "/Fo$taskBuild/test.obj" "/Fe$taskBuild/test.exe" "$PSScriptRoot/auxiliary_present.cpp"
if($LASTEXITCODE){throw 'Auxiliary-present fixture compilation failed'}
& "$taskBuild/test.exe"
if($LASTEXITCODE){throw 'Auxiliary-present regression failed'}
[IO.File]::WriteAllText("$taskBuild/auxiliary_present_production.inl",'')
& cl /nologo /std:c++20 /EHsc "/I$taskBuild" "/Fo$taskBuild/old.obj" "/Fe$taskBuild/old.exe" "$PSScriptRoot/auxiliary_present.cpp"
if($LASTEXITCODE){throw 'Negative-control compilation failed'}
& "$taskBuild/old.exe"
if($LASTEXITCODE -ne 1){throw 'Missing auxiliary bypass was not detected'}
Write-Output 'PASS: negative control detects the old duplicate hidden-frame bookkeeping path'
