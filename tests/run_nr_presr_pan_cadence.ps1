# Run in a Visual Studio developer shell. Exercises extracted production code.
$ErrorActionPreference='Stop'
$taskRepo=(Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$taskBuild=Join-Path ([IO.Path]::GetTempPath()) ('presr-pan-'+[guid]::NewGuid())
New-Item -ItemType Directory -Path $taskBuild | Out-Null
$taskSource=[IO.File]::ReadAllText((Join-Path $taskRepo 'OptiScaler/shaders/dlssnr/DlssNr_DeferredSr.inl'))
function Fragment([string]$start,[string]$end,[int]$offset=0) {
    $a=$taskSource.IndexOf($start,$offset)
    if($a -lt 0) { throw "Missing production fragment: $start" }
    $b=$taskSource.IndexOf($end,$a)
    if($b -lt 0) { throw "Missing production terminator: $end" }
    return $taskSource.Substring($a,$b-$a)
}
$taskRecord=Fragment '    const bool testMotion =' '    Use use(g, cmd);'
$taskArm=Fragment '            p->Set(NVSDK_NGX_Parameter_Reset,' '            p->Set(NVSDK_NGX_Parameter_Jitter_Offset_X,'
$taskAfter=Fragment '    if (evaluateSr)' '    if (g.reset)' ($taskSource.IndexOf('void After('))
$taskFg=Fragment '(split ? !h.split.schedule.HasEdit() : !h.havePrevious) || UInt(g.parameters,' ', h.anchorId++);'
$taskProduction="void RecordProduction(Generation& g,bool privateJob,unsigned flags,bool wantsHalf,unsigned long long motionNow) {`n"+
    $taskRecord+"}`nvoid ArmProduction(Generation& g,Frame frame) { auto* p=g.parameters; bool panReset=false;`n"+
    $taskArm+"}`nvoid EvaluateProduction(Generation& g,Pair pair) { bool evaluateSr=!pair.skipNr; void* cmd=nullptr; struct { unsigned slot=0; } use;`n"+
    $taskAfter+"}`nbool ResetFgProduction(Generation& g) { bool split=false; auto& h=*g.half; return "+$taskFg+"; }`n"
[IO.File]::WriteAllText((Join-Path $taskBuild 'nr_presr_pan_production.inl'),$taskProduction)
& cl.exe /nologo /std:c++20 /EHsc /UNDEBUG "/I$taskRepo/OptiScaler" "/I$taskBuild" "/Fo$taskBuild/pan.obj" "/Fe$taskBuild/pan.exe" "$PSScriptRoot/nr_presr_pan_cadence.cpp"
if($LASTEXITCODE -ne 0) { throw 'PreSR pan regression compilation failed.' }
& "$taskBuild/pan.exe"
if($LASTEXITCODE -ne 0) { throw 'PreSR pan regression failed.' }
