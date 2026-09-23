# Run from a Visual Studio developer shell. Requires Windows Graphics Tools;
# uses the WARP adapter, with no NVIDIA runtime or running game.
$ErrorActionPreference = 'Stop'
$taskRepo = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$taskBuild = Join-Path ([IO.Path]::GetTempPath()) ('presr-buffers-' + [guid]::NewGuid())
New-Item -ItemType Directory -Path $taskBuild | Out-Null
$taskSource = [IO.File]::ReadAllText((Join-Path $taskRepo 'OptiScaler/shaders/dlssnr/DlssNr_DeferredSr.inl'))
function Read-Fragment([string]$start, [string]$end) {
    $taskStart = $taskSource.IndexOf($start)
    if ($taskStart -lt 0) { throw "Production fragment changed: $start" }
    $taskEnd = $taskSource.IndexOf($end, $taskStart)
    if ($taskEnd -lt 0) { throw "Production fragment changed: $end" }
    return $taskSource.Substring($taskStart, $taskEnd - $taskStart)
}
$taskCapture = (Read-Fragment '    auto* cleanTarget = half ?' '    ID3D12Resource* base = cleanTarget;').Replace('pair.output', 'output')
$taskRotate = Read-Fragment '        std::swap(h.motion, h.previousMotion);' '        h.havePrevious = ok;'
$taskProduction = "ID3D12Resource* CaptureProduction(Generation& g, ID3D12GraphicsCommandList* cmd, ID3D12Resource* output, bool half, bool split=false) {`n" +
    $taskCapture + "`nreturn cleanTarget;`n}`nvoid RotateProduction(HalfRate& h) {`n" + $taskRotate + "`n}`n"
$taskProduction += Read-Fragment 'bool SnapshotSplitGuides(' 'bool PrepareHalfRate('
$taskSelection=Read-Fragment '    ID3D12Resource* rejectionReference = nullptr;' '    bool ok = true;'
$taskProduction += "`nID3D12Resource* SelectRejectionProduction(Generation& g,bool split,unsigned mode,ID3D12Resource* cleanTarget,float scale,float& exposure) { struct {unsigned Mode; float ReferencePreExposure=0;unsigned ResidualRejection=0;} apply{mode}; struct {float scale;} pair{scale}; const unsigned DlssNrMode_ApplyInterpolatedResidual=10;`n"+$taskSelection+"`nexposure=apply.ReferencePreExposure;return rejectionReference;}`n"
[IO.File]::WriteAllText((Join-Path $taskBuild 'presr-buffer-production.inl'), $taskProduction)
& cl.exe /nologo /std:c++20 /EHsc /UNDEBUG /DNOMINMAX "/I$taskRepo/OptiScaler" "/I$taskBuild" "/Fo$taskBuild/presr_buffers.obj" "/Fe$taskBuild/presr_buffers.exe" "$PSScriptRoot/presr_buffers.cpp" d3d12.lib dxgi.lib
if ($LASTEXITCODE -ne 0) { throw 'PreSR buffer test compilation failed.' }
& "$taskBuild/presr_buffers.exe"
if ($LASTEXITCODE -ne 0) { throw 'PreSR buffer/timestamp regression failed.' }
