@echo off
chcp 65001 >nul
cd /d "%~dp0"

REM Продовжити трену RVC Bender з чекпоінта.
REM ОБОВ'ЯЗКОВО закрий run_rvc.bat / Applio UI — інакше 4070 Ti не вистачить VRAM.

set "APPLIO=A:\Programs\Applio-main\Applio-main"
set "PY=%APPLIO%\env\python.exe"
set "MODEL=Bender"
set "EPOCHS=500"
set "SAVE_EVERY=10"
set "SR=48000"
set "BATCH=2"

set "OUT=%~dp0models"
set "LOGS=%APPLIO%\logs\%MODEL%"

if not exist "%PY%" (
  echo Немає Applio python: %PY%
  pause
  exit /b 1
)
if not exist "%LOGS%\G_2333333.pth" if not exist "%LOGS%\G_*.pth" (
  echo Немає чекпоінта в %LOGS%
  echo Спочатку один раз препроцес+extract у Applio.
  pause
  exit /b 1
)

echo.
echo Закрий колонку (run_rvc.bat), потім Enter.
pause

echo.
echo Train %MODEL% → %EPOCHS% епох, сейв кожні %SAVE_EVERY%.
echo Resume з існуючого G/D, без cleanup.
echo.

cd /d "%APPLIO%"
"%PY%" core.py train --model-name "%MODEL%" --vocoder "HiFi-GAN" --sample-rate %SR% --batch-size %BATCH% --total-epoch %EPOCHS% --save-every-epoch %SAVE_EVERY% --save-every-weights --gpu 0 --pretrained --index-algorithm Auto --checkpointing
if errorlevel 1 (
  echo Трену впало. Дивись лог вище.
  pause
  exit /b 1
)

echo.
echo Копіюю найсвіжіші ваги в voice_clone\models\
if not exist "%OUT%" mkdir "%OUT%"

powershell -NoProfile -Command ^
  "$d='%LOGS%'; $out='%OUT%'; $w=Get-ChildItem -LiteralPath $d -Filter 'Bender_*e_*.pth' | Sort-Object LastWriteTime -Descending | Select-Object -First 1; if(-not $w){ throw 'немає Bender_*e_*.pth' }; Copy-Item -LiteralPath $w.FullName -Destination (Join-Path $out 'bender.pth') -Force; Write-Host ('pth: '+$w.Name); $i=Get-ChildItem -LiteralPath $d -Filter '*.index' | Sort-Object LastWriteTime -Descending | Select-Object -First 1; if($i){ Copy-Item -LiteralPath $i.FullName -Destination (Join-Path $out 'bender.index') -Force; Write-Host ('index: '+$i.Name) }"

echo.
echo Готово. Запусти run_rvc.bat. Тримай експорти 150/200/300 —
echo після якогось моменту зайві епохи можуть перетренувати голос.
echo.
echo Щоб голос став реально живим: допиши ще 8–10 хв пародії в raw\ і знову prepare_samples + трену.
pause
