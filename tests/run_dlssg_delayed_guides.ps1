$ErrorActionPreference='Stop'
$taskRepo=Split-Path $PSScriptRoot
$taskOut=Join-Path $env:TEMP ('fg-guides-'+[guid]::NewGuid())
New-Item -ItemType Directory $taskOut | Out-Null
$taskH=[IO.File]::ReadAllText((Join-Path $taskRepo 'OptiScaler/framegen/dlssg/DLSSG_Dx12.h'))
$taskA=$taskH.IndexOf('    struct GuideSnapshot');$taskB=$taskH.IndexOf('    bool Dispatch();',$taskA)
$taskMembers=$taskH.Substring($taskA,$taskB-$taskA).Replace('    TemporalContinuity::SuccessfulFrames _sceneHistory;','')
[IO.File]::WriteAllText((Join-Path $taskOut 'guide-members.inl'),$taskMembers)
$taskC=[IO.File]::ReadAllText((Join-Path $taskRepo 'OptiScaler/framegen/dlssg/DLSSG_Dx12.cpp'))
$taskA=$taskC.IndexOf('void DLSSG_Dx12::RetireGuide(');$taskB=$taskC.IndexOf('bool DLSSG_Dx12::SetResource(',$taskA)
[IO.File]::WriteAllText((Join-Path $taskOut 'guide-methods.inl'),$taskC.Substring($taskA,$taskB-$taskA))
& cl.exe /nologo /std:c++20 /EHsc /UNDEBUG "/I$taskOut" "/Fo$taskOut/test.obj" "/Fe$taskOut/test.exe" "$PSScriptRoot/dlssg_delayed_guides.cpp"
if($LASTEXITCODE -ne 0){throw 'Guide test compile failed'}
& "$taskOut/test.exe"
if($LASTEXITCODE -ne 0){throw 'Guide history test failed'}
