# VS developer shell; creates hidden WARP swapchains, no game or NVIDIA SDK required.
$ErrorActionPreference='Stop'
$taskRepo=Split-Path $PSScriptRoot
$taskBuild=Join-Path $env:TEMP ('dxgi-resize-flags-'+[guid]::NewGuid())
New-Item -ItemType Directory -Path $taskBuild | Out-Null
# Ensure both native and interop entry points reach the tested production policy.
foreach($taskFile in @('wrapped/wrapped_swapchain.cpp','with_dx12/dx11_with_dx12_sc.cpp')){
 $taskSource=[IO.File]::ReadAllText("$taskRepo/OptiScaler/$taskFile")
 if(([regex]::Matches($taskSource,'PreserveSwapchainResizeFlags\(')).Count -ne 2){throw "Both resize entry points must preserve flags: $taskFile"}
}
& cl /nologo /std:c++20 /EHsc /W4 /UNDEBUG "/Fo$taskBuild/test.obj" "/Fe$taskBuild/test.exe" "$PSScriptRoot/dxgi_resize_flags.cpp" d3d12.lib dxgi.lib user32.lib
if($LASTEXITCODE){throw 'DXGI resize flag fixture compilation failed'}
& "$taskBuild/test.exe"
if($LASTEXITCODE){throw 'DXGI resize flag regression failed'}
