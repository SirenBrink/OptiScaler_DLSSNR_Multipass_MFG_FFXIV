# Run in a Visual Studio developer shell. No game or NVIDIA runtime required.
$ErrorActionPreference='Stop'
$taskRepo=(Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$taskBuild=Join-Path ([IO.Path]::GetTempPath()) ('hdr-preview-'+[guid]::NewGuid())
New-Item -ItemType Directory -Path $taskBuild | Out-Null
& cl.exe /nologo /std:c++20 /EHsc "/I$taskRepo/OptiScaler" "/Fo$taskBuild/" "/Fe$taskBuild/preview.exe" "$PSScriptRoot/hdr_preview_dx11.cpp"
if($LASTEXITCODE -ne 0){throw 'HDR preview test compilation failed'}
& "$taskBuild/preview.exe"
if($LASTEXITCODE -ne 0){throw 'Native HDR preview coverage test failed'}
