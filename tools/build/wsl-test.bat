@echo off
rem Configure + build the test preset and run unit tests, from a WSL shell:
rem    cmd.exe /c tools\build\wsl-test.bat
cd /d "%~dp0..\.."
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
cmake --preset windows-msvc-test || exit /b 1
cmake --build --preset windows-msvc-test || exit /b 1
ctest --preset unit --output-on-failure
