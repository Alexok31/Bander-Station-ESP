@echo off
chcp 65001 >nul
:: Нужен Admin один раз: пускает TCP 8765 с ESP на этот ПК.
net session >nul 2>&1
if %errorlevel% neq 0 (
  echo Запрос прав администратора для Windows Firewall...
  powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
  exit /b
)

netsh advfirewall firewall delete rule name="Bender Local Voice WS 8765" >nul 2>&1
netsh advfirewall firewall add rule name="Bender Local Voice WS 8765" dir=in action=allow protocol=TCP localport=8765 profile=any
if errorlevel 1 (
  echo Не удалось открыть порт 8765.
  pause
  exit /b 1
)

echo.
echo OK: порт 8765 открыт для ESP.
echo Ethernet «Space Lovers» лучше поставить Private:
echo   Параметры → Сеть и Интернет → Ethernet → Частная сеть
echo.
pause
