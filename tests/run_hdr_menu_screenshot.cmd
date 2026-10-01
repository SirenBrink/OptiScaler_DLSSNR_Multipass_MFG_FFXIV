@echo off
setlocal
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
if not "%errorlevel%"=="0" exit /b 1
if not exist "%~dp0out" mkdir "%~dp0out"
cl /nologo /std:c++20 /EHsc /W4 /MD "%~dp0hdr_menu_screenshot.cpp" /Fo"%~dp0out\hdr_menu_screenshot.obj" /Fe"%~dp0out\hdr_menu_screenshot.exe"
if not "%errorlevel%"=="0" exit /b 1
"%~dp0out\hdr_menu_screenshot.exe" "%~dp0out\hdr-roundtrip.png"
exit /b %errorlevel%
