@echo off
chcp 65001 >nul
cd /d "%~dp0"

REM Продовжити з epoch 20 (G_2600). БЕЗ --cleanup, інакше зітре чекпоінт.
REM Закрий run_rvc.bat.

set "APPLIO=A:\Programs\Applio-main\Applio-main"
set "PY=%APPLIO%\env\python.exe"
set "MODEL=Bender"
set "SR=48000"
set "EPOCHS=120"
set "SAVE_EVERY=10"
set "BATCH=2"
set "OUT=%~dp0models"
set "LOGS=%APPLIO%\logs\%MODEL%"

if not exist "%LOGS%\G_2600.pth" (
  echo Немає G_2600.pth — немає з чого resume.
  exit /b 1
)

echo Resume Bender 20e → %EPOCHS%e, без cleanup
cd /d "%APPLIO%"
"%PY%" core.py train --model-name "%MODEL%" --vocoder "HiFi-GAN" --sample-rate %SR% --batch-size %BATCH% --total-epoch %EPOCHS% --save-every-epoch %SAVE_EVERY% --save-every-weights --gpu 0 --pretrained --index-algorithm Auto --checkpointing
if errorlevel 1 (
  echo Train fail
  exit /b 1
)

if not exist "%OUT%" mkdir "%OUT%"
powershell -NoProfile -Command ^
  "$d='%LOGS%'; $out='%OUT%'; $w=Get-ChildItem -LiteralPath $d -Filter 'Bender_*e_*.pth' | Where-Object { $_.LastWriteTime -gt (Get-Date '2026-08-28') } | Sort-Object LastWriteTime -Descending | Select-Object -First 1; if(-not $w){ $w=Get-ChildItem -LiteralPath $d -Filter 'Bender_*e_*.pth' | Sort-Object LastWriteTime -Descending | Select-Object -First 1 }; Copy-Item -LiteralPath $w.FullName -Destination (Join-Path $out 'bender.pth') -Force; Write-Host ('pth: '+$w.Name); $i=Get-ChildItem -LiteralPath $d -Filter '*.index' | Sort-Object LastWriteTime -Descending | Select-Object -First 1; if($i){ Copy-Item -LiteralPath $i.FullName -Destination (Join-Path $out 'bender.index') -Force; Write-Host ('index: '+$i.Name) }"

echo Готово. Запусти run_rvc.bat
pause
