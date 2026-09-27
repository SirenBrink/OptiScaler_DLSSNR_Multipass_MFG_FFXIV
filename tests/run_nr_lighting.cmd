@echo off
setlocal
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
if not exist "%~dp0out" mkdir "%~dp0out"
for %%t in (nr_lighting_history nr_lighting_readback nr_lighting_recovery) do (
    cl /nologo /std:c++20 /EHsc /DNOMINMAX /UNDEBUG "%~dp0%%t.cpp" /Fo"%~dp0out\%%t.obj" /Fe"%~dp0out\%%t.exe"
    if errorlevel 1 exit /b 1
    "%~dp0out\%%t.exe"
    if errorlevel 1 exit /b 1
)
