# Обучение прототипа голосового вызова

Состояние на 2026-10-04: текущий кандидат `prototype-v7-finetune` на `dataset-v6`.
Модель обучена локально; на колонке runtime ещё не установлен. Пользовательские
записи не отправлялись во внешние сервисы.
Диагностический runtime уже добавлен в исходники и собирается вместе с
прошивкой; включение и аппаратная проверка описаны в [wake-runtime.md](wake-runtime.md).

## Актуальный результат: prototype-v7-finetune

Всего 169 реальных записей: 35 «Привет, Бендер», 28 «Эй, Бендер», 23 «Бендер»,
83 отрицательных. Последние две партии добавили 38 отрицательных исходников.
До проверки результатов 26 выделены в обучение, 12 — в новый test. Все
прежние разбиения заморожены. Реальные источники: 118 train, 14 validation,
37 test (25 ранее проверенных и 12 новых); с синтетикой — 12 004 обучающих окна.

Прежняя модель сработала на 10/26 новых development-файлов. Обучение с нуля
`prototype-v7-baseline` не прошло ограничение validation и отклонено до
проверки нового test. Затем `prototype-v7-finetune` дообучена от весов
`prototype-v6-baseline` с learning rate 0,00015 и замороженными слоями
BatchNormalization. Порог 0,25 и выбранные веса зафиксированы по прежней
validation. План и выбор сохранены в `iteration-v7-plan.json` и
`selection-v7.json` до просмотра нового test.

Результаты готовой INT8-модели (17 408 байт):

- Прежний test обращений: 14/15, включая «Привет, Бендер» 5/5,
  «Эй, Бендер» 4/5, «Бендер» 5/5. Эти файлы не обучающие, но их результаты
  уже были известны по прошлой модели: это проверка регрессий.
- Прежний test фона: 0/10 ложных срабатываний.
- Свежий test фона: 1/12 ложных, 48 секунд. Предыдущая модель на **тех же
  12 файлах** давала 2/12 при своём ранее зафиксированном пороге.
- Суммарно test фона: 1/22 на 88 секундах. Это не оценка числа ошибок в час.
- Validation: 8/9 реальных и 30/30 синтетических обращений; 0/5 и 0/22 ложных.

Новая модель уменьшила ошибки на добавленном фоне, сохранив результат на
прежних обращениях, но не подтверждает готовность постоянного прослушивания.
Порог по новому test не подбирался. `ready_for_device` остаётся false.
9 проверок подготовки и разбиения данных прошли. Диагностический runtime
на ESP32-S3 добавлен, frontend проверен на ПК. Следующий этап — замеры
RAM/времени и совместной работы с AirPlay на плате, затем голосовой запуск диалога.

[Актуальный отчёт](../artifacts/wake-training/prototype-v7-finetune/REPORT.md)
содержит результаты, JSON проверки и сравнение на одинаковом фоне.

```powershell
& ./artifacts/wake-training/venv/Scripts/python.exe agent-tools/prepare_wake_training.py --dataset dataset-v6 --freeze-splits dataset-v5 --reserve-test-negative 12 --stream-windows
& ./artifacts/wake-training/venv/Scripts/python.exe agent-tools/train_wake_prototype.py --dataset dataset-v6 --run prototype-v7-finetune --stream-selection --initial-run prototype-v6-baseline --learning-rate .00015 --freeze-batchnorm --epochs 30
& ./artifacts/wake-training/venv/Scripts/python.exe agent-tools/evaluate_wake_holdout.py --run prototype-v7-finetune --previous-test-dataset dataset-v5
& ./artifacts/wake-training/venv/Scripts/python.exe agent-tools/finalize_wake_prototype.py --run prototype-v7-finetune
```

Для повторения использовать новые имена папок, чтобы не затереть исходные результаты.

## История: prototype-v6-baseline

В каноническом наборе 131 запись с колонки: 35 «Привет, Бендер», 28 «Эй,
Бендер», 23 «Бендер», 45 отрицательных. Все скачанные партии сохранены;
скачивание не удаляло записи с устройства. Дополнительно используются прежние
340 синтетических исходников. Веса INT8 занимают 17 408 байт.

До новых прогонов выделены 25 свежих исходников: по 5 каждого обращения и
10 отрицательных. Они отсутствуют в обучающих окнах, шумовых добавках,
калибровке INT8 и validation. Старое разбиение 55 записей заморожено, чтобы
при добавлении партий прежние проверочные файлы не попадали в обучение.
Итого реальные записи: 92 train, 14 validation, 25 test.

Сравнены четыре новых варианта (обычное/сохраняющее временную структуру
окончание сети, с дополнительными окнами реального контекста и без них).
Выбор зафиксирован в `artifacts/wake-training/selection-v6.json` **до** проверки
test: прохождение ограничений validation, затем 0,75 AUC реальных + 0,25 AUC
синтетических записей, при равенстве меньший размер. Test проверен только для
выбранной модели; порог 0,4140625 взят из validation и после test не менялся.

Результат на новых отложенных файлах:

- «Привет, Бендер»: 5/5.
- «Эй, Бендер»: 4/5.
- «Бендер»: 5/5.
- Фон: 0 из 10 записей вызвали срабатывание, всего 40 секунд фона.

На прежней validation: 8/9 реальных обращений и 0/5 ложных; синтетические —
30/30 и 0/22. Сохранение окружающего фразу звука и добавление причинных окон
начала записи улучшило результат по сравнению с теми же данными без этих
окон. После добавления окон используется 10 132 обучающих примера.

Это **не доказательство готовности постоянного прослушивания**: короткие
записи одного человека не оценивают надёжность в течение часов, на других
голосах или во время музыки. `ready_for_device` остаётся false. Следующий
этап — портирование frontend/runtime, проверка DSP, ресурсов и длительного
фона на колонке, без подстройки порога под уже проверенный test.

Актуальные [отчёт](../artifacts/wake-training/prototype-v6-baseline/REPORT.md),
[проверка новых записей](../artifacts/wake-training/prototype-v6-baseline/holdout.json)
и ZIP находятся в `artifacts/wake-training/prototype-v6-baseline`.
9 проверок подготовки данных/разбиения прошли; INT8-файл проверен по SHA,
форме входа и отсутствию float-тензоров.

Команды последнего эксперимента (требуют исходный `dataset-v2` и текущий индекс;
при повторении нужно выбрать новые имена выходных папок):

```powershell
& ./artifacts/wake-training/venv/Scripts/python.exe agent-tools/prepare_wake_training.py --dataset dataset-v4 --freeze-splits dataset-v2 --reserve-test-per-phrase 5 --reserve-test-negative 10
& ./artifacts/wake-training/venv/Scripts/python.exe agent-tools/prepare_wake_training.py --dataset dataset-v5 --freeze-splits dataset-v4 --stream-windows
& ./artifacts/wake-training/venv/Scripts/python.exe agent-tools/train_wake_prototype.py --dataset dataset-v5 --run prototype-v6-baseline --stream-selection --epochs 45
& ./artifacts/wake-training/venv/Scripts/python.exe agent-tools/evaluate_wake_holdout.py --run prototype-v6-baseline
& ./artifacts/wake-training/venv/Scripts/python.exe agent-tools/finalize_wake_prototype.py --run prototype-v6-baseline
```

Ниже сохранено описание первого эксперимента и исходного окружения.

## История: результат prototype-v3

Модель `artifacts/wake-training/prototype-v3/bender.int8.tflite` занимает
17 408 байт, содержит 6 085 параметров. При диагностическом пороге 0,5:

- Реальные обращения: 7 из 9; «Эй, Бендер» 2/3, «Привет, Бендер» 3/4, «Бендер» 2/2.
- Реальный фон: ложное срабатывание на 2 из 5 записей, всего 20 секунд фона.
- Синтетические обращения: 29/30; ложное срабатывание на 7 из 22 отрицательных записей.

Не найден порог, одновременно дающий минимум 8/9 реальных, 24/30 синтетических
обращений и ноль ложных срабатываний на validation. Поэтому `threshold: null`,
`ready_for_device: false`. Диагностический порог нельзя считать настройкой
для прошивки. Максимальное расхождение float/INT8 по вероятности на проверочных
окнах составляет около 0,186; его также нужно учитывать при улучшении модели.
Размер файла не является оценкой нужной RAM или времени обработки на ESP32.

Подробные вероятности, метрики и ограничения находятся в `report.json`,
история обучения — в `history.json`, описание модели — в `model.json`.
`REPORT.md` и `bender-prototype.zip` создаёт финализатор. ZIP не содержит
пользовательских аудиозаписей.

## Данные и проверка

Канонический индекс: `artifacts/wakeword-device/dataset-index.json`.
55 записей с колонки: 17 «Привет, Бендер», 12 «Эй, Бендер», 7 «Бендер»,
19 отрицательных. Последняя партия целиком исправлена на отрицательную по
указанию пользователя; используется архив с суффиксом `-negative.zip`.
Исходные ошибочные метки сохранены для происхождения данных.

Дополнительно локально синтезированы 340 исходников Piper: два голоса
denis/dmitri, 180 обращений и 160 примеров обычной/похожей речи. URL и SHA-256
голосов зафиксированы в `agent-tools/wake-voice-sources.json`. Карточки данных
обоих голосов указывают CC0. Синтетика не доказывает работу на других людях.

До преобразований отложены 14 реальных записей (9 обращений, 5 отрицательных)
и 52 синтетические (30 обращений, 22 отрицательных). Аугментации исходника
остаются в одном наборе. Одинаковый отрицательный синтетический текст обоими
голосами не пересекает train/validation. Проверочные записи не используются
для примешивания шума или калибровки INT8. После преобразований — 3 732
обучающих окна. Исходные SHA и разбиение сохранены в `dataset-v2/manifest.json`.

Validation использовалась для выбора весов и оценки порогов. Это не
независимый финальный тест: реальный голос один, записи из близких сессий.
20 секунд фона недостаточно для оценки ложных вызовов за час. Следующий этап
требует длинного фона, обычной речи, похожих слов, музыки и новых обращений
из отдельной сессии. Для универсальной модели нужны и другие реальные голоса.

## Обработка звука

Это собственный DS-CNN с признаками `bender-logmel-v1`, **не готовая модель
microWakeWord или WakeNet**. Контракт описан в `model.json` и
`agent-tools/wake_training_audio.py`:

- PCM mono, 16 kHz; исходные 24 kHz преобразуются полифазным ресемплером.
- Окно 2,4 секунды, кадр 30 мс, шаг 10 мс, FFT 512, 40 mel-полос 125–7500 Hz.
- Симметричное окно Hann, натуральный логарифм мощности, фиксированная нормализация.
- Вход INT8 формы `[1,238,40,1]`, выход INT8 — вероятность любого из трёх обращений.
- Проверка полного звука скользящим окном каждые 100 мс; три положительных решения подряд.

Обрезка по границам речи применяется только при подготовке положительных
обучающих примеров; потоковая проверка не вырезает фразу из записи.
`frontend_reference.npz` содержит коэффициенты и контрольный пример для
будущего сравнения с DSP на плате. Нужно реализовать такой же frontend,
проверить память, задержку, энергопотребление и совместную работу с AirPlay.

## Воспроизведение

Команды выполняются из корня `firmware`. Обучение использует отдельный
CPython 3.12 и не меняет зависимости рабочего AI-сервера. В текущей рабочей
копии отдельный интерпретатор лежит в `artifacts/wake-training/python312/tools`.

```powershell
python3.12 -m venv artifacts/wake-training/venv
& ./artifacts/wake-training/venv/Scripts/python.exe -m pip install -r agent-tools/requirements-wake-training.txt
python agent-tools/download_wake_voices.py
```

Для генерации нужен отдельный интерпретатор с `piper-tts==1.7.0`, numpy,
scipy и onnxruntime. Здесь использован уже установленный Python 3.14 с Piper;
рабочий сервер при этом не запускался и не перезапускался. Далее `python`
обозначает именно интерпретатор с Piper:

```powershell
python agent-tools/generate_wake_synthetic.py
& ./artifacts/wake-training/venv/Scripts/python.exe agent-tools/prepare_wake_training.py --dataset dataset-v2
& ./artifacts/wake-training/venv/Scripts/python.exe agent-tools/train_wake_prototype.py --dataset dataset-v2 --run prototype-v3 --stream-selection --epochs 35
& ./artifacts/wake-training/venv/Scripts/python.exe -m unittest discover -s tests -p test_wake_training.py
& ./artifacts/wake-training/venv/Scripts/python.exe agent-tools/finalize_wake_prototype.py --run prototype-v3
```

Сохранённые WAV и SHA определяют точные синтетические исходники: шум синтеза
Piper при повторной генерации не гарантированно побитово одинаков. Обучение
фиксирует seed, однако аппаратные и библиотечные отличия также могут менять
результат. Для нового эксперимента указывать новое имя `--run`, сохраняя
предыдущие веса и отчёты. `dataset-v1` и `prototype-v1/v2` — исторические
эксперименты; актуальны `dataset-v2` и `prototype-v3`.

## Первичные источники

- [TensorFlow: распознавание аудиокоманд](https://www.tensorflow.org/tutorials/audio/simple_audio)
- [LiteRT: целочисленная квантизация](https://ai.google.dev/edge/litert/models/post_training_integer_quant)
- [microWakeWord: отдельный frontend и обучение](https://github.com/OHF-Voice/micro-wake-word)
- [Карточка голоса denis](https://huggingface.co/rhasspy/piper-voices/blob/main/ru/ru_RU/denis/medium/MODEL_CARD)
- [Карточка голоса dmitri](https://huggingface.co/rhasspy/piper-voices/blob/main/ru/ru_RU/dmitri/medium/MODEL_CARD)
