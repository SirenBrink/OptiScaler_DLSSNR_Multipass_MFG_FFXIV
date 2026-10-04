$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$build=Join-Path $env:TEMP ('external-hudless-'+[guid]::NewGuid())
New-Item -ItemType Directory -Path $build | Out-Null
$extract=Get-Content -Raw (Join-Path $repo 'OptiScaler/shaders/ui_extract/UE_Common.h')
$produce=Get-Content -Raw (Join-Path $repo 'OptiScaler/misc/UiPaste.cpp')
$a=[regex]::Match($extract,'ueShaderCode = R"\((.*?)\)";', 'Singleline')
$b=[regex]::Match($produce,'ProduceShaderCode = R"\((.*?)\)";', 'Singleline')
if(!$a.Success -or !$b.Success){throw 'Production shaders not found'}
[IO.File]::WriteAllText("$build/extract.hlsl",$a.Groups[1].Value)
[IO.File]::WriteAllText("$build/produce.hlsl",$b.Groups[1].Value)
& cl.exe /nologo /std:c++20 /EHsc /UNDEBUG "/Fo$build/test.obj" "/Fe$build/test.exe" "$PSScriptRoot/external_hudless_warp.cpp" d3d11.lib d3dcompiler.lib
if($LASTEXITCODE){throw 'HUDless test compilation failed'}
& "$build/test.exe" "$build/extract.hlsl" "$build/produce.hlsl"
if($LASTEXITCODE){throw 'HUDless tests failed'}
