@echo off
chcp 65001 >nul
cd /d "%~dp0"

set "PY=C:\Users\Admin\AppData\Local\Python\pythoncore-3.14-64\python.exe"
if not exist "%PY%" set "PY=python"

REM --- RVC: своя пародія Бендера (див. voice_clone\README.md) ---
if not exist "A:\tmp" mkdir "A:\tmp"
set "TEMP=A:\tmp"
set "TMP=A:\tmp"
set "TMPDIR=A:\tmp"
if not defined STANZA_RESOURCES_DIR set "STANZA_RESOURCES_DIR=A:\stanza_resources"

set RVC_ENABLE=1
if not defined OLLAMA_MODEL set "OLLAMA_MODEL=aya-expanse:8b"
REM Папка Applio, де є core.py:
if not defined APPLIO_ROOT set "APPLIO_ROOT=A:\Programs\Applio-main\Applio-main"
if not defined APPLIO_PYTHON set "APPLIO_PYTHON=A:\Programs\Applio-main\Applio-main\env\python.exe"
REM Модель після тренування:
if not defined RVC_PTH set "RVC_PTH=voice_clone\models\bender.pth"
if not defined RVC_INDEX set "RVC_INDEX=voice_clone\models\bender.index"
if not defined RVC_PITCH set "RVC_PITCH=-1"
if not defined RVC_INDEX_RATE set "RVC_INDEX_RATE=0.55"
if not defined RVC_RMS set "RVC_RMS=0.75"
if not defined RVC_PROTECT set "RVC_PROTECT=0.5"

echo.
echo RVC_ENABLE=%RVC_ENABLE%
echo LLM: config.json → "llm": "local" або "grok", "bender_level": 1–10
echo APPLIO_ROOT=%APPLIO_ROOT%
echo APPLIO_PYTHON=%APPLIO_PYTHON%
echo RVC_PTH=%RVC_PTH%
echo.
echo 1) Спочатку записи + prepare_samples + трену в Applio.
echo 2) open_firewall.bat від адміністратора (один раз).
echo 3) Колонка: PTT як завжди.
echo.

"%PY%" server.py
if errorlevel 1 pause
pause
