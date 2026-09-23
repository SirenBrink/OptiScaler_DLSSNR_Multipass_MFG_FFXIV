$ErrorActionPreference='Stop'
$taskRepo=Split-Path $PSScriptRoot
$taskBuild=Join-Path $env:TEMP ('xefg-integration-'+[guid]::NewGuid())
New-Item -ItemType Directory -Path $taskBuild | Out-Null
foreach($taskPair in @(@('XeFGPacing.h','pacing-production.h'),@('XeFGUnlock.h','unlock-production.h'))){
 $taskText=[IO.File]::ReadAllText((Join-Path $taskRepo ('OptiScaler/proxies/'+$taskPair[0])))
 $taskText=[regex]::Replace($taskText,'(?m)^#include "(?:SysUtils|Logger|Config|XeFGPacing)\.h"\r?\n','')
 [IO.File]::WriteAllText((Join-Path $taskBuild $taskPair[1]),$taskText)
}
& cl.exe /nologo /std:c++20 /EHsc /UNDEBUG "/I$taskBuild" "/Fo$taskBuild/test.obj" "/Fe$taskBuild/test.exe" "$PSScriptRoot/xefg_mfg_integration.cpp"
if($LASTEXITCODE -ne 0){throw 'XeMFG integration compile failed'}
& "$taskBuild/test.exe"
if($LASTEXITCODE -ne 0){throw 'XeMFG integration regression failed'}
