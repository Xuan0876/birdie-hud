@echo off
rem FPV 覆盖层：先开 MSFS（无边框全屏）和 ChasePlane，再双击这个
cd /d "%~dp0"
python overlay.py %*
pause
