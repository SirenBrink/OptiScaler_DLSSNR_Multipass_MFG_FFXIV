param([string]$NvapiInclude)
$ErrorActionPreference='Stop'
$taskRepo=Split-Path $PSScriptRoot
if(!$NvapiInclude){$NvapiInclude=Join-Path $taskRepo 'external/nvapi'}
$taskBuild=Join-Path $env:TEMP ('nr-reuse-api-'+[guid]::NewGuid())
New-Item -ItemType Directory -Path $taskBuild | Out-Null
$taskSource=[IO.File]::ReadAllText("$taskRepo/OptiScaler/dlssnr/DlssNrNative.cpp")
function Extract([string]$signature){
 $start=$taskSource.IndexOf($signature);if($start -lt 0){throw "Missing $signature"}
 $open=$taskSource.IndexOf('{',$start);$end=$open+1;$depth=1
 while($depth -gt 0 -and $end -lt $taskSource.Length){if($taskSource[$end] -eq '{'){$depth++};if($taskSource[$end] -eq '}'){$depth--};$end++}
 if($depth){throw 'Unclosed function'}
 $taskSource.Substring($start,$end-$start)
}
$taskExtract=(Extract 'template<class Kernel>')+"`n"+(Extract 'NvAPI_Status __cdecl LaunchEx(')+"`n"+(Extract 'void BeginEvaluate(')
[IO.File]::WriteAllText("$taskBuild/nr_reuse_api_production.inl",$taskExtract)
& cl /nologo /std:c++20 /EHsc /W4 "/I$NvapiInclude" "/I$taskBuild" "/Fo$taskBuild/test.obj" "/Fe$taskBuild/test.exe" "$PSScriptRoot/nr_reuse_api.cpp"
if($LASTEXITCODE -ne 0){throw 'Reuse API compile failed'}
& "$taskBuild/test.exe"
if($LASTEXITCODE -ne 0){throw 'Reuse API regression failed'}
# Negative control: the previous wrapper held this mutex around driver calls.
$taskBad=$taskExtract.Replace('auto&s=S();const auto driver=s.launchEx.load();','auto&s=S();std::lock_guard<std::recursive_mutex> oldLock(s.mutex);const auto driver=s.launchEx.load();')
[IO.File]::WriteAllText("$taskBuild/nr_reuse_api_production.inl",$taskBad)
& cl /nologo /std:c++20 /EHsc "/I$NvapiInclude" "/I$taskBuild" "/Fo$taskBuild/legacy.obj" "/Fe$taskBuild/legacy.exe" "$PSScriptRoot/nr_reuse_api.cpp"
if($LASTEXITCODE -ne 0){throw 'Negative control compile failed'}
& "$taskBuild/legacy.exe"
if($LASTEXITCODE -ne 1){throw 'Old global lock was not rejected'}
Write-Output 'PASS: negative control detects the previous startup lock scope'
# Negative control: a full downstream pass used to clear every feature's cache.
$taskBad=$taskExtract.Replace('s.vit.Invalidate(feature);','s.vit.Clear();')
[IO.File]::WriteAllText("$taskBuild/nr_reuse_api_production.inl",$taskBad)
& cl /nologo /std:c++20 /EHsc "/I$NvapiInclude" "/I$taskBuild" "/Fo$taskBuild/global-clear.obj" "/Fe$taskBuild/global-clear.exe" "$PSScriptRoot/nr_reuse_api.cpp"
if($LASTEXITCODE -ne 0){throw 'Global-clear negative control compile failed'}
& "$taskBuild/global-clear.exe"
if($LASTEXITCODE -ne 1){throw 'Global cache clearing was not rejected'}
Write-Output 'PASS: negative control detects downstream passes cancelling first-pass reuse'
