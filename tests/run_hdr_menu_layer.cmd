@echo off
setlocal
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
if not "%errorlevel%"=="0" exit /b 1
if not exist "%~dp0out" mkdir "%~dp0out"
pushd "%~dp0out"
cl /nologo /DIMGUI_USER_CONFIG=\"imgui_geometry_config.h\" /I"%~dp0." /std:c++20 /EHsc /W4 /MD "%~dp0hdr_menu_layer.cpp" "%~dp0../OptiScaler/include/imgui/imgui.cpp" "%~dp0../OptiScaler/include/imgui/imgui_draw.cpp" "%~dp0../OptiScaler/include/imgui/imgui_tables.cpp" "%~dp0../OptiScaler/include/imgui/imgui_widgets.cpp"  /Fe"%~dp0out\hdr_menu_layer.exe"
if not "%errorlevel%"=="0" exit /b 1
"%~dp0out\hdr_menu_layer.exe"
exit /b %errorlevel%
