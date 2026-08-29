@echo off
chcp 65001 >nul
cd /d "%~dp0"

REM Новий датасет (~10 хв) — preprocess + extract + трену З НУЛЯ (pretrained).
REM Не resume з 500e: той голос був перетренований.
REM Закрий run_rvc.bat, інакше 4070 Ti не вистачить VRAM.

set "APPLIO=A:\Programs\Applio-main\Applio-main"
set "PY=%APPLIO%\env\python.exe"
set "MODEL=Bender"
set "SR=48000"
set "EPOCHS=120"
set "SAVE_EVERY=10"
set "BATCH=2"
set "DATASET=%~dp0dataset"
set "OUT=%~dp0models"
set "LOGS=%APPLIO%\logs\%MODEL%"

if not exist "%PY%" (
  echo Немає Applio python: %PY%
  exit /b 1
)
if not exist "%DATASET%\bender_001.wav" (
  echo Немає датасету. Спочатку: python voice_clone\prepare_samples.py
  exit /b 1
)

echo.
echo [1/3] Preprocess %DATASET%
cd /d "%APPLIO%"
"%PY%" core.py preprocess --model-name "%MODEL%" --dataset-path "%DATASET%" --sample-rate %SR% --cut-preprocess Automatic --chunk-len 3.0 --overlap-len 0.3
if errorlevel 1 (
  echo Preprocess fail
  exit /b 1
)

echo.
echo [2/3] Extract f0/features
"%PY%" core.py extract --model-name "%MODEL%" --sample-rate %SR% --f0-method rmvpe --embedder-model contentvec --gpu 0
if errorlevel 1 (
  echo Extract fail
  exit /b 1
)

echo.
echo [3/3] Train %EPOCHS% e, save every %SAVE_EVERY%, --cleanup (не продовжуємо 500e)
"%PY%" core.py train --model-name "%MODEL%" --vocoder "HiFi-GAN" --sample-rate %SR% --batch-size %BATCH% --total-epoch %EPOCHS% --save-every-epoch %SAVE_EVERY% --save-every-weights --gpu 0 --pretrained --cleanup --index-algorithm Auto --checkpointing
if errorlevel 1 (
  echo Train fail
  exit /b 1
)

echo.
echo Копіюю ваги в voice_clone\models\
if not exist "%OUT%" mkdir "%OUT%"
powershell -NoProfile -Command ^
  "$d='%LOGS%'; $out='%OUT%'; $w=Get-ChildItem -LiteralPath $d -Filter 'Bender_*e_*.pth' | Sort-Object LastWriteTime -Descending | Select-Object -First 1; if(-not $w){ throw 'немає Bender_*e_*.pth' }; Copy-Item -LiteralPath $w.FullName -Destination (Join-Path $out 'bender.pth') -Force; Write-Host ('pth: '+$w.Name); $i=Get-ChildItem -LiteralPath $d -Filter '*.index' | Sort-Object LastWriteTime -Descending | Select-Object -First 1; if($i){ Copy-Item -LiteralPath $i.FullName -Destination (Join-Path $out 'bender.index') -Force; Write-Host ('index: '+$i.Name) }"

echo.
echo Готово. Запусти run_rvc.bat. Якщо 120e знову «п'яне» — постав Bender_80e_* з logs\Bender.
echo.
pause
