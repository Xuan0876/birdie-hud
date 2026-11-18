@echo off
rem Configure, build and run the parity tests with MSVC x64 + Ninja (both ship with Visual Studio 2022).
rem Usage: scripts\build.bat [release^|debug]
setlocal
set PRESET=%1
if "%PRESET%"=="" set PRESET=release
set "VSINSTALLER=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer"
rem vcvars looks for vswhere on PATH as well
set "PATH=%VSINSTALLER%;%PATH%"
for /f "usebackq delims=" %%i in (`call vswhere.exe -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (
  echo Visual Studio 2022 with the C++ tools was not found.
  exit /b 1
)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d "%~dp0.."
cmake --preset %PRESET% || exit /b 1
cmake --build --preset %PRESET% || exit /b 1
ctest --preset %PRESET% || exit /b 1
