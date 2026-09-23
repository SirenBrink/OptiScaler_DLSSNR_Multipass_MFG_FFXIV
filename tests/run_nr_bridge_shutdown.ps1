param([string]$NgxInclude)
# Run from a Visual Studio developer shell. No NVIDIA runtime or game is needed.
$ErrorActionPreference = 'Stop'
$taskRepo = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
if (!$NgxInclude) { $NgxInclude = Join-Path $taskRepo 'external/nvngx_dlss_sdk' }
$taskBuild = Join-Path ([IO.Path]::GetTempPath()) ('nr-shutdown-' + [guid]::NewGuid())
New-Item -ItemType Directory -Path $taskBuild | Out-Null
function Read-Function([string]$source, [string]$name) {
    $start = $source.IndexOf("void $name()")
    if ($start -lt 0) { throw "Missing production function: $name" }
    $open = $source.IndexOf('{', $start)
    $depth = 1
    $end = $open + 1
    while ($depth -gt 0 -and $end -lt $source.Length) {
        if ($source[$end] -eq '{') { $depth++ }
        if ($source[$end] -eq '}') { $depth-- }
        $end++
    }
    if ($depth -ne 0) { throw "Incomplete production function: $name" }
    return $source.Substring($start, $end - $start)
}
$taskDeferred = [IO.File]::ReadAllText((Join-Path $taskRepo 'OptiScaler/shaders/dlssnr/DlssNr_DeferredSr.inl'))
$taskNr = [IO.File]::ReadAllText((Join-Path $taskRepo 'OptiScaler/shaders/dlssnr/DlssNr_Dx12.cpp'))
# Use the real definitions through Collect(), omitting rendering functions that
# require a model. The test exercises real ownership/destructors, not a copy.
$taskEnd = $taskDeferred.IndexOf('unsigned UInt(')
if ($taskEnd -lt 0) { throw 'Production generation definitions changed; update test extraction.' }
$taskProduction = $taskDeferred.Substring(0, $taskEnd) + (Read-Function $taskDeferred 'Shutdown') + "`n}`n" +
    (Read-Function $taskNr 'SuspendForBridgeShutdown') + "`n" + (Read-Function $taskNr 'ResumeAfterBridgeInit')
$taskInclude = Join-Path $taskBuild 'nr_shutdown_production.inl'
[IO.File]::WriteAllText($taskInclude, $taskProduction)
function Build-Test([string]$name) {
    & cl.exe /nologo /std:c++20 /EHsc /UNDEBUG /DNOMINMAX "/I$NgxInclude" "/I$taskRepo/OptiScaler" "/I$taskBuild" "/Fo$taskBuild/$name.obj" "/Fe$taskBuild/$name.exe" "$PSScriptRoot/nr_bridge_shutdown.cpp"
    if ($LASTEXITCODE -ne 0) { throw 'NR shutdown test compilation failed.' }
}
Build-Test 'current'
& "$taskBuild/current.exe"
if ($LASTEXITCODE -ne 0) { throw 'NR shutdown lifecycle test failed.' }
& "$taskBuild/current.exe" 'late'
if ($LASTEXITCODE -ne 0) { throw 'NR ownership called NGX during CRT teardown.' }
# Negative control: restore precisely the original two global owning containers.
# The same late-runtime test must detect the original exit-time driver callback.
$taskLegacy = $taskProduction.Replace('std::unique_ptr<Generation>& current = *new std::unique_ptr<Generation>;', 'std::unique_ptr<Generation> current;').Replace('std::vector<std::unique_ptr<Generation>>& retired = *new std::vector<std::unique_ptr<Generation>>;', 'std::vector<std::unique_ptr<Generation>> retired;')
if ($taskLegacy -ceq $taskProduction) { throw 'Negative control no longer matches production ownership.' }
[IO.File]::WriteAllText($taskInclude, $taskLegacy)
Build-Test 'legacy'
& "$taskBuild/legacy.exe" 'late'
if ($LASTEXITCODE -ne 99) { throw 'Negative control failed to reproduce the late NGX release.' }
Write-Output 'CRT teardown regression passed; original ownership reproduced the late NGX release (99).'
