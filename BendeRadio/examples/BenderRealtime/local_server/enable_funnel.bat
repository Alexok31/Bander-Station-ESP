@echo off
chcp 65001 >nul
:: Один раз включает Tailscale Funnel: ПК становится сервером с постоянным именем.
:: Колонка ходит на desktop-ov51efm.tail2312b4.ts.net из любой Wi‑Fi.

set "TS=C:\Program Files\Tailscale\tailscale.exe"
if not exist "%TS%" (
  echo Tailscale не найден: %TS%
  pause
  exit /b 1
)

echo.
echo 1) В браузере откроется страница Tailscale — нажми Enable Funnel.
echo 2) Потом этот скрипт пропишет порт 8765.
echo.
start "" "https://login.tailscale.com/f/funnel?node=nNNRx68YW121CNTRL"
echo Когда Funnel включён в аккаунте, нажми любую клавишу здесь...
pause >nul

"%TS%" funnel reset >nul 2>&1
"%TS%" funnel --bg 8765
if errorlevel 1 (
  echo Не удалось включить Funnel. Проверь, что нажал Enable на сайте.
  pause
  exit /b 1
)

echo.
"%TS%" funnel status
echo.
echo Готово. Постоянный адрес колонки:
echo   wss://desktop-ov51efm.tail2312b4.ts.net/v1/realtime
echo.
echo Дальше: start.bat ^(голос-сервер^), прошей колонку с новым secrets.h.
echo Cloudflare-туннель больше не нужен.
echo.
pause
