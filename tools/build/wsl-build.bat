@echo off
rem Build helper for driving the MSVC build from a WSL shell:
rem    cmd.exe /c tools\build\wsl-build.bat [preset]
cd /d "%~dp0..\.."
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
set PRESET=%1
if "%PRESET%"=="" set PRESET=windows-msvc-debug
cmake --build --preset %PRESET%
