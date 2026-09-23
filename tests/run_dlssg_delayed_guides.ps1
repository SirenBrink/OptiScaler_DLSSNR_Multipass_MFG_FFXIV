$ErrorActionPreference='Stop'
$taskRepo=Split-Path $PSScriptRoot
$taskOut=Join-Path $env:TEMP ('fg-guides-'+[guid]::NewGuid())
New-Item -ItemType Directory $taskOut | Out-Null
$taskH=[IO.File]::ReadAllText((Join-Path $taskRepo 'OptiScaler/framegen/IFGFeature_Dx12.h'))
$taskA=$taskH.IndexOf('    struct GuideSnapshot');$taskB=$taskH.IndexOf('    // End presentation guide history.',$taskA)
$taskMembers=$taskH.Substring($taskA,$taskB-$taskA).Replace('    TemporalContinuity::SuccessfulFrames _sceneHistory;','')
[IO.File]::WriteAllText((Join-Path $taskOut 'guide-members.inl'),$taskMembers)
$taskC=[IO.File]::ReadAllText((Join-Path $taskRepo 'OptiScaler/framegen/IFGFeature_Dx12.cpp'))
$taskA=$taskC.IndexOf('void IFGFeature_Dx12::RetireGuide(');$taskB=$taskC.Length
[IO.File]::WriteAllText((Join-Path $taskOut 'guide-methods.inl'),$taskC.Substring($taskA,$taskB-$taskA).Replace('IFGFeature_Dx12::','DLSSG_Dx12::'))
& cl.exe /nologo /std:c++20 /EHsc /UNDEBUG "/I$taskOut" "/Fo$taskOut/test.obj" "/Fe$taskOut/test.exe" "$PSScriptRoot/dlssg_delayed_guides.cpp"
if($LASTEXITCODE -ne 0){throw 'Guide test compile failed'}
& "$taskOut/test.exe"
if($LASTEXITCODE -ne 0){throw 'Guide history test failed'}
