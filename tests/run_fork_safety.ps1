$ErrorActionPreference='Stop'
$taskRepo=Split-Path $PSScriptRoot
$taskBuild=Join-Path $env:TEMP ('fork-safety-'+[guid]::NewGuid())
New-Item -ItemType Directory -Path $taskBuild | Out-Null
function Read-Function([string]$source,[string]$signature) {
    $start=$source.IndexOf($signature)
    if($start -lt 0){throw "Missing production function: $signature"}
    $open=$source.IndexOf('{',$start);$depth=1;$end=$open+1
    while($depth -gt 0 -and $end -lt $source.Length){
        if($source[$end] -eq '{'){$depth++}
        if($source[$end] -eq '}'){$depth--}
        $end++
    }
    if($depth){throw 'Incomplete production function'}
    $source.Substring($start,$end-$start)
}
$taskFg=[IO.File]::ReadAllText("$taskRepo/OptiScaler/framegen/IFGFeature_Dx12.cpp")
$taskBridge=[IO.File]::ReadAllText("$taskRepo/OptiScaler/with_dx12/dx11_with_dx12_sc.cpp")
$taskProduction=(Read-Function $taskFg 'void IFGFeature_Dx12::CancelPendingUpscalerWork()')+"`n"+
    (Read-Function $taskFg 'bool IFGFeature_Dx12::WaitForUIAllocator(UINT index)')+"`n"+
    (Read-Function $taskFg 'bool IFGFeature_Dx12::WaitForSCAllocator(UINT index)')+"`n"+
    (Read-Function $taskFg 'bool IFGFeature_Dx12::SubmitSCCommandList(UINT index)')+"`n"+
    (Read-Function $taskBridge 'bool Dx11wDx12SC::_WaitForCopyAllocator(UINT slot)')+"`n"+
    (Read-Function $taskBridge 'bool Dx11wDx12SC::_WaitForCopyQueueIdle()')
function Build-Test([string]$name,[string]$source) {
    [IO.File]::WriteAllText("$taskBuild/fork_safety_production.inl",$source)
    & cl /nologo /std:c++20 /EHsc /W4 /MD "/I$taskBuild" "/Fo$taskBuild/$name.obj" "/Fe$taskBuild/$name.exe" "$PSScriptRoot/fork_safety.cpp"
    if($LASTEXITCODE -ne 0){throw 'Safety regression compile failed'}
}
Build-Test current $taskProduction
& "$taskBuild/current.exe"
if($LASTEXITCODE -ne 0){throw 'Safety regression failed'}
# Reproduce the old cancellation order independently of the fence tests.
$taskLegacy=$taskProduction.Replace('    Deactivate();','').Replace('    UINT cancelled = 0;',"    Deactivate();`n    UINT cancelled = 0;")
Build-Test legacy $taskLegacy
& "$taskBuild/legacy.exe"
if($LASTEXITCODE -ne 1){throw 'Cancellation negative control did not catch the old submission'}
Write-Output 'PASS: negative control reproduced the original cancel-before-exit submission bug'
