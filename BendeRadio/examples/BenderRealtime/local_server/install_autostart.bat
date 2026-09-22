@echo off
chcp 65001 >nul
cd /d "%~dp0"

set "LNK=%APPDATA%\Microsoft\Windows\Start Menu\Programs\Startup\BenderVoice.lnk"
set "TARGET=%~dp0start.bat"
set "WORKDIR=%~dp0"

powershell -NoProfile -Command ^
  "$s = (New-Object -COM WScript.Shell).CreateShortcut($env:LNK);" ^
  "$s.TargetPath = 'cmd.exe';" ^
  "$s.Arguments = '/k \"\"' + $env:TARGET + '\" always';" ^
  "$s.WorkingDirectory = $env:WORKDIR;" ^
  "$s.WindowStyle = 1;" ^
  "$s.Save()"

if errorlevel 1 (
  echo Не удалось добавить автозапуск.
  pause
  exit /b 1
)

echo.
echo ОК: при входе в Windows запустится голос-сервер + туннель.
echo ПК должен быть включён и в интернете.
echo Ярлык: %LNK%
echo Убрать: удали этот ярлык из Автозагрузки.
echo.
pause
