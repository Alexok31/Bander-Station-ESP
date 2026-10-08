@echo off
cd /d "%~dp0"
chcp 65001 >nul

if /i "%~1"=="always" set "NO_PAUSE=1"

set "PYTHON_BASIC_REPL=1"
set "PYTHONUNBUFFERED=1"
set "PYTHONDONTWRITEBYTECODE=1"

set "PY=C:\Users\Admin\AppData\Local\Python\pythoncore-3.14-64\python.exe"
if not exist "%PY%" set "PY=python"

if not exist "%~dp0server.py" (
  echo ERROR: server.py not found in %~dp0
  pause
  exit /b 1
)

echo.
echo Stopping old Bender on port 8765 if any...
for /f "tokens=5" %%P in ('netstat -ano ^| findstr ":8765" ^| findstr "LISTENING"') do (
  taskkill /F /PID %%P >nul 2>&1
)
timeout /t 2 /nobreak >nul

if not exist "A:\tmp" mkdir "A:\tmp"
set "TEMP=A:\tmp"
set "TMP=A:\tmp"
set "TMPDIR=A:\tmp"
if not defined STANZA_RESOURCES_DIR set "STANZA_RESOURCES_DIR=A:\stanza_resources"

set "RVC_ENABLE=1"
if not defined OLLAMA_MODEL set "OLLAMA_MODEL=aya-expanse:8b"
if not defined APPLIO_ROOT set "APPLIO_ROOT=A:\Programs\Applio-main\Applio-main"
if not defined APPLIO_PYTHON set "APPLIO_PYTHON=A:\Programs\Applio-main\Applio-main\env\python.exe"
if not defined RVC_PTH set "RVC_PTH=voice_clone\models\bender.pth"
if not defined RVC_INDEX set "RVC_INDEX=voice_clone\models\bender.index"
if not defined RVC_PITCH set "RVC_PITCH=-3"
if not defined RVC_INDEX_RATE set "RVC_INDEX_RATE=0.55"
if not defined RVC_RMS set "RVC_RMS=0.75"
if not defined RVC_PROTECT set "RVC_PROTECT=0.33"

echo.
echo Bender voice server + RVC
echo Funnel: https://desktop-ov51efm.tail2312b4.ts.net
echo Python: %PY%
echo Wait for: RVC OK   and   listen ... auth=basic
echo Keep this window open.
echo.

"%PY%" -B -u "%~dp0server.py"
set "ERR=%ERRORLEVEL%"
echo.
echo Server stopped, exit code %ERR%
if not "%NO_PAUSE%"=="1" pause
exit /b %ERR%
