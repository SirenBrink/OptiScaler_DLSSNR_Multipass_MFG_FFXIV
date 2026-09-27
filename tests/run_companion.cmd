@echo off
setlocal
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
if not exist "%~dp0out" mkdir "%~dp0out"
cl /nologo /std:c++20 /EHsc /W4 /MD "%~dp0companion_core_test.cpp" /Fo"%~dp0out\companion_core_test.obj" /Fe"%~dp0out\companion_core_test.exe"
if errorlevel 1 exit /b 1
"%~dp0out\companion_core_test.exe"
if errorlevel 1 exit /b 1
cl /nologo /std:c++20 /EHsc /W4 /MD /LD "%~dp0companion_abi_fixture.cpp" /Fo"%~dp0out\companion_abi_fixture.obj" /Fe"%~dp0out\CompanionBridgeFixture.dll" /link /IMPLIB:"%~dp0out\CompanionBridgeFixture.lib"
if errorlevel 1 exit /b 1
cl /nologo /std:c++20 /EHsc /W4 /MD "%~dp0companion_markers_test.cpp" /Fo"%~dp0out\companion_markers_test.obj" /Fe"%~dp0out\companion_markers_test.exe"
if errorlevel 1 exit /b 1
"%~dp0out\companion_markers_test.exe"
if errorlevel 1 exit /b 1
cl /nologo /std:c++20 /EHsc /W4 /MD "%~dp0companion_packets_test.cpp" /Fo"%~dp0out\companion_packets_test.obj" /Fe"%~dp0out\companion_packets_test.exe"
if errorlevel 1 exit /b 1
"%~dp0out\companion_packets_test.exe"
if errorlevel 1 exit /b 1
cl /nologo /std:c++20 /EHsc /W4 /MD "%~dp0companion_geometry_test.cpp" /Fo"%~dp0out\companion_geometry_test.obj" /Fe"%~dp0out\companion_geometry_test.exe"
if errorlevel 1 exit /b 1
"%~dp0out\companion_geometry_test.exe"
if errorlevel 1 exit /b 1
cl /nologo /std:c++20 /EHsc /W4 /MD "%~dp0companion_layer_test.cpp" /Fo"%~dp0out\companion_layer_test.obj" /Fe"%~dp0out\companion_layer_test.exe" /link user32.lib
if errorlevel 1 exit /b 1
"%~dp0out\companion_layer_test.exe"
if errorlevel 1 exit /b 1
cl /nologo /std:c++20 /EHsc /W4 /MD "%~dp0companion_midpoint_test.cpp" /Fo"%~dp0out\companion_midpoint_test.obj" /Fe"%~dp0out\companion_midpoint_test.exe"
if errorlevel 1 exit /b 1
"%~dp0out\companion_midpoint_test.exe"
if errorlevel 1 exit /b 1
cl /nologo /std:c++20 /EHsc /W4 /MD "%~dp0companion_visibility_test.cpp" /Fo"%~dp0out\companion_visibility_test.obj" /Fe"%~dp0out\companion_visibility_test.exe"
if errorlevel 1 exit /b 1
"%~dp0out\companion_visibility_test.exe"
if errorlevel 1 exit /b 1
cl /nologo /std:c++20 /EHsc /W4 /MD "%~dp0companion_image_midpoint_test.cpp" /Fo"%~dp0out\companion_image_midpoint_test.obj" /Fe"%~dp0out\companion_image_midpoint_test.exe"
if errorlevel 1 exit /b 1
"%~dp0out\companion_image_midpoint_test.exe"
exit /b %errorlevel%
