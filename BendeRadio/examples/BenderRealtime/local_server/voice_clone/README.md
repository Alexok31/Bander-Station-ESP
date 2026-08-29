# Голос Бендера з твоєї пародії (RVC)

XTTS для української майже не підходить — тому схема така:

**Piper (український текст → мова)** → **RVC (тембр твоєї пародії)** → колонка.

## 1. Запиши пародію

1. Відкрий `phrases_uk.txt` — читай у голосі Бендера (ліниво, грубо, «метал»).
2. Запис: телефон / Audacity / Windows Voice Recorder.
3. Тиша в кімнаті, мікрофон 15–20 см, без музики.
4. **5–15 хвилин** чистого голосу (краще кілька дублів).
5. Поклади файли в `voice_clone/raw/`.

## 2. Підготуй датасет

З папки `local_server`:

```bat
python voice_clone\prepare_samples.py
```

З’являться `voice_clone/dataset/bender_XXX.wav` (mono 48 kHz).
Потрібен [ffmpeg](https://ffmpeg.org/download.html) у PATH (для mp3/m4a).

## 3. Навчи RVC в Applio

1. Встанови [Applio](https://github.com/IAHispano/Applio) (свій Python/venv, не 3.14 сервера).
2. Train:
   - **model_name**: `bender`
   - **dataset_path**: повний шлях до `local_server/voice_clone/dataset`
   - sample rate **48000**
3. Після трену (зазвичай сотні–тисячі кроків) експортуй / скопіюй:
   - `bender.pth` (або `G_xxxx.pth` → перейменуй)
   - `*.index` якщо є  
   → в `local_server/voice_clone/models/`

Перевірив у Applio Inference на будь-якому wav — має звучати як твоя пародія.

Зараз у проєкті **~2.5 хв** датасету і модель з **2 епох** — тому тембр сирий.

Щоб дотренувати **без нових записів** (продовжить з чекпоінта до 120 епох):

1. Закрий `run_rvc.bat` (VRAM).
2. Запусти `voice_clone\train_more.bat`.
3. Знову `run_rvc.bat`.

Це покращить схожість, але стеля лишається в кількості запису. Для нормальної пародії треба **8–15 хвилин** того самого голосу в `raw\`, потім знову `prepare_samples.py` і трену (уже з `--cleanup` / нова модель, бо датасет змінився).

## 4. Увімкни в Bender-сервері

У `run_rvc.bat` уже прописані змінні, або вручну перед `server.py`:

```bat
set RVC_ENABLE=1
set APPLIO_ROOT=A:\Programs\Applio-main\Applio-main
set RVC_PTH=voice_clone\models\bender.pth
set RVC_INDEX=voice_clone\models\bender.index
set RVC_PITCH=0
```

`APPLIO_ROOT` — папка, де лежить `core.py`.

Перезапусти сервер. У логах має бути: `RVC OK (...)`.
Якщо `RVC skip` — модель або шлях Applio не знайдені, Piper грає як раніше.

## Поради по пародії

- Трохи нижче звичайного тону, «в нос/метал», не кричи.
- Одна манера на всіх фразах — RVC тоді стабільніший.
- Не клоди епізоди «Футурами» — лише **твій** голос.
- Якщо тембр «плаває»: більше хвилин запису, або `RVC_INDEX_RATE=0.75`, `RVC_PITCH=-2`.

## Чому не XTTS

XTTS v2 погано тримає українську фонетику. Piper вже вміє uk; RVC лише підміняє голос — це якраз те, що треба колонці.
