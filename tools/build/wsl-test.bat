@echo off
rem Configure + build the test preset and run unit tests, from a WSL shell:
rem    cmd.exe /c "$(wslpath -w .)/tools/build/wsl-test.bat"
rem Build tree on the Windows side; see wsl-build.bat for why.
setlocal
set SRC=%~dp0..\..
set BUILD=%LOCALAPPDATA%\GrimVault\build\windows-msvc-test

call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1

cd /d %LOCALAPPDATA%
cmake -S "%SRC%" --preset windows-msvc-test -B "%BUILD%" -DVCPKG_INSTALLED_DIR=C:\v\i || exit /b 1
cmake --build "%BUILD%" || exit /b 1
ctest --test-dir "%BUILD%" -L unit --output-on-failure
