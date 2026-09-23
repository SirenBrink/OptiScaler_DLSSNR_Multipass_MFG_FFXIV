# Run in a Visual Studio developer shell; no game, GPU or NVIDIA runtime needed.
$ErrorActionPreference='Stop'
$taskRepo=(Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$taskBuild=Join-Path ([IO.Path]::GetTempPath()) ('presr-guides-'+[guid]::NewGuid())
New-Item -ItemType Directory -Path $taskBuild | Out-Null
$taskSource=[IO.File]::ReadAllText((Join-Path $taskRepo 'OptiScaler/shaders/dlssnr/DlssNr_DeferredSr.inl'))
$taskStart=$taskSource.IndexOf('    frame.RenderSubrectWidth = g.w;')
if($taskStart -lt 0) { throw 'Production PreSR frame metadata moved.' }
$taskEnd=$taskSource.IndexOf('    frame.ColourIsLinearHdr =',$taskStart)
if($taskEnd -lt 0) { throw 'Production PreSR metadata terminator moved.' }
$taskFragment=$taskSource.Substring($taskStart,$taskEnd-$taskStart)
$taskProduction="DlssNrFrameInfo BuildProduction(unsigned w,unsigned h,unsigned outW,unsigned outH,unsigned flags,unsigned guideSourceWidth=0,unsigned guideSourceHeight=0) {`n"+
    "struct { unsigned w,h,outW,outH; } g{w,h,outW,outH}; DlssNrFrameInfo frame {};`n"+$taskFragment+"return frame; }`n"
$taskInclude=Join-Path $taskBuild 'nr_presr_guides_production.inl'
function Build-Test([string]$name) {
    & cl.exe /nologo /std:c++20 /EHsc "/I$taskRepo/OptiScaler" "/I$taskBuild" "/Fo$taskBuild/$name.obj" "/Fe$taskBuild/$name.exe" "$PSScriptRoot/nr_presr_guide_metadata.cpp"
    if($LASTEXITCODE -ne 0) { throw 'PreSR guide regression compilation failed.' }
}
[IO.File]::WriteAllText($taskInclude,$taskProduction)
Build-Test 'current'
& "$taskBuild/current.exe"
if($LASTEXITCODE -ne 0) { throw 'PreSR guide regression failed.' }
# Negative control: the actual old caller omitted these metadata assignments.
$taskLegacy=$taskProduction.Replace('    frame.MotionVectorsLowResolution = (flags & NVSDK_NGX_DLSS_Feature_Flags_MVLowRes) != 0;', '').Replace('    frame.OutputWidth = g.outW; frame.OutputHeight = g.outH;', '')
if($taskLegacy -ceq $taskProduction) { throw 'Negative control no longer matches production.' }
[IO.File]::WriteAllText($taskInclude,$taskLegacy)
Build-Test 'legacy'
& "$taskBuild/legacy.exe"
if($LASTEXITCODE -ne 1) { throw 'Negative control did not detect the original padded-MV bug.' }
Write-Output 'Negative control reproduced the original PreSR padded-motion metadata error.'
