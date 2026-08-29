@echo off
chcp 65001 >nul
cd /d "A:\Programs\Applio-main\Applio-main"

if not exist "A:\tmp" mkdir "A:\tmp"
if not exist "A:\uv-cache" mkdir "A:\uv-cache"
if not exist "A:\pip-cache" mkdir "A:\pip-cache"

set "UV_CACHE_DIR=A:\uv-cache"
set "PIP_CACHE_DIR=A:\pip-cache"
set "TEMP=A:\tmp"
set "TMP=A:\tmp"
set "TMPDIR=A:\tmp"
set "HF_HOME=A:\hf-cache"

echo.
echo Applio install into:
echo   %cd%
echo Cache on A: (C: almost full)
echo.

if not exist "env\python.exe" (
  echo ERROR: env\python.exe not found.
  echo Run Applio run-install.bat once to create the conda env, then this script again.
  pause
  exit /b 1
)

echo [1/2] uv...
"env\python.exe" -m pip install -U uv
if errorlevel 1 (
  echo pip install uv failed
  pause
  exit /b 1
)

echo.
echo [2/2] dependencies + PyTorch CUDA 12.8 ^(долго, 10-30 мин^)...
"env\python.exe" -m uv pip install -r requirements.txt --python "%cd%\env\python.exe" --extra-index-url https://download.pytorch.org/whl/cu128 --index-strategy unsafe-best-match
if errorlevel 1 (
  echo uv pip failed
  pause
  exit /b 1
)

echo.
echo OK. Дальше: run-applio.bat  (откроет браузер)
echo После трену модели: Bender run_rvc.bat
echo.
pause
