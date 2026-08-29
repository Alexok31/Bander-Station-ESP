@echo off
chcp 65001 >nul
cd /d "%~dp0"

set "PY=C:\Users\Admin\AppData\Local\Python\pythoncore-3.14-64\python.exe"
if not exist "%PY%" set "PY=python"

echo.
echo 1) Сначала один раз запусти open_firewall.bat от имени администратора.
echo 2) Сервер должен печатать: listen ws://0.0.0.0:8765
echo    Потом на колонке PTT.
echo.
"%PY%" server.py
if errorlevel 1 pause
pause
