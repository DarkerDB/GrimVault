@echo off
rem Build from the WSL-hosted source tree:
rem    cmd.exe /c "$(wslpath -w .)/tools/build/wsl-build.bat" [preset]
rem
rem cmd.exe cannot cd to a UNC path and MSVC link steps spawn cmd, so the
rem build tree lives on the Windows side (%LOCALAPPDATA%\GrimVault\build);
rem only sources are read over \\wsl.localhost. CI keeps the presets'
rem default ${sourceDir}/build layout (its checkout is already on C:).
setlocal
set SRC=%~dp0..\..
set PRESET=%1
if "%PRESET%"=="" set PRESET=windows-msvc-debug
set BUILD=%LOCALAPPDATA%\GrimVault\build\%PRESET%

call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1

cd /d %LOCALAPPDATA%
cmake -S "%SRC%" --preset %PRESET% -B "%BUILD%" -DVCPKG_INSTALLED_DIR=C:\v\i || exit /b 1
cmake --build "%BUILD%"
