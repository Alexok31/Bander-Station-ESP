# Локальный Бендер (ПК + ESP32)

Колонка шлёт PCM на этот компьютер. Здесь: **Whisper** (голос→текст) → **Ollama/Qwen** (мозг) → **Piper** (текст→голос).

## 1. На ПК (4070 Ti)

1. Установи [Ollama](https://ollama.com), в PowerShell:

   ```bat
   ollama pull qwen2.5:7b
   ```

   Для умнее (больше VRAM): `ollama pull qwen2.5:14b` и в `server.py` / env `OLLAMA_MODEL=qwen2.5:14b`.

2. Python 3.11/3.12, затем в этой папке один раз: `python -m pip install -r requirements.txt`.

   Дальше всегда один файл: **`start.bat`**. Enter = сервер + туннель.
   Чтобы поднималось само после входа в Windows: **`install_autostart.bat`**.
   Первый запуск качает Whisper и голос Piper.

3. Узнай IP: `ipconfig` → **IPv4** адаптера Wi‑Fi (например `192.168.1.42`).  
   **Не** `127.0.0.1` — колонка не достучится до «самого себя» ПК.

4. Windows Firewall: **один раз** запусти `open_firewall.bat` **от имени администратора**.
   Без этого ESP пишет `connect failed` (~3 с) — сеть Ethernet часто в профиле **Public**, и входящий 8765 режется.
   Ещё лучше: Параметры → Сеть → Ethernet → **частная сеть**.

Дождись в консоли: `listen ws://0.0.0.0:8765/v1/realtime`.
При PTT в этом же окне должно появиться `handshake /v1/realtime from ('192.168.0.x', …)`.
Если handshake нет — колонка до ПК не достучалась (файрвол / не тот IP).

## 2. Прошивка

В `secrets.h`:

```c
#define AI_PROVIDER "local"
#define LOCAL_WS_HOST "192.168.1.42"  // твой IPv4
#define LOCAL_WS_PORT 8765
```

Прошей как обычно. PTT тот же: зажал — говори, отпустил — ответ.

Сокет требует логин/пароль из того же `secrets.h`:

```c
#define LOCAL_WS_USER "bender"
#define LOCAL_WS_PASS "shiny-metal-76"
```

ПК читает их при старте `server.py`. Сменил пароль — перепрошей колонку и перезапусти сервер.

Вернуть облако: `#define AI_PROVIDER "xai"`.

## Из любой сети (ПК дома всегда сервер)

Колонка не тянет Whisper/RVC сама. Сервер — домашний ПК + **Tailscale Funnel** (постоянное имя).

1. Tailscale установлен и вошёл в аккаунт.
2. Один раз `enable_funnel.bat` — на сайте нажми **Enable Funnel**.
3. `start.bat` (голос-сервер). ПК не выключай / `install_autostart.bat`.
4. Прошей колонку. В `secrets.h` уже:
   `LOCAL_WS_PUBLIC_HOST "desktop-ov51efm.tail2312b4.ts.net"`
5. Дома — LAN. В другом городе — любая Wi‑Fi, адрес копировать не нужно.

Без домашнего ПК (другой голос): `AI_PROVIDER "xai"`.

## Голос «як Бендер» (твоя пародія + RVC)

XTTS для української слабкий. Схема: **Piper (мова)** → **RVC (твій тембр)**.

Коротко:

1. Начитай `voice_clone/phrases_uk.txt` у характері Бендера → файли в `voice_clone/raw/`
2. `python voice_clone\prepare_samples.py`
3. Навчи модель у [Applio](https://github.com/IAHispano/Applio), поклади `.pth` / `.index` у `voice_clone/models/`
4. Пропиши `APPLIO_ROOT` і запускай `start.bat`

Деталі: [voice_clone/README.md](voice_clone/README.md).

## Если не стартует Whisper на GPU

Сервер сам попробует CPU. Для CUDA: свежий драйвер NVIDIA. Модель легче: переменная `WHISPER_MODEL=medium`.
