@echo off
rem Build hudgfx.dll (x64) into ..\python, next to the reference Python files.
setlocal
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d "%~dp0"
cl /nologo /O2 /EHsc /std:c++17 /LD /MT /W3 hudgfx.cpp /Fe:..\python\hudgfx.dll /link /NOLOGO || exit /b 1
del /q hudgfx.obj ..\python\hudgfx.exp ..\python\hudgfx.lib 2>nul
echo built ..\python\hudgfx.dll
