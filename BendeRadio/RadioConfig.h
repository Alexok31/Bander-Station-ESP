#pragma once

#include <cstdint>

// Все настройки пинов, Wi‑Fi и таймингов «холодного» старта в одном месте.
class RadioConfig {
   public:
    // Значения по умолчанию, если в NVS ещё ничего не сохраняли (первый запуск / сброс).
    static constexpr const char* wifiSsid = "Space Lovers";
    static constexpr const char* wifiPass = "cassini31";

    // Точка доступа ESP для настройки: подключитесь к ней и откройте http://192.168.4.1
    static constexpr const char* apSsid = "Bender Station";
    // Пароль AP не короче 8 символов (WPA2). Пустая строка = открытая сеть (только для отладки).
    static constexpr const char* apPassDefault = "benderadio";

    // Плата Bender V3 (ESP32-S3-N16R8): I2S → два MAX98357A (L+R). Пины как myoptions.h / bender.cpp.
    static constexpr uint8_t i2sDout = 21;
    static constexpr uint8_t i2sBclk = 44;
    static constexpr uint8_t i2sLrc = 2;
    // Как yoRadio: MUTE_PIN=41, MUTE_VAL=HIGH когда плеер остановлен; при play — !MUTE_VAL.
    // false снова «всегда unmute» (хуже для щелчков на SD).
    static constexpr bool ampUseHardwareMute = true;
    static constexpr uint8_t ampMutePin = 41;
    static constexpr bool ampMuteWhenIdleHigh = true;  // MUTE_VAL HIGH
    static constexpr uint16_t ampUnmuteAfterStreamMs = 700;
    static constexpr int8_t ampVolumeUiMax = 21;
    // Стерео: L и R через SD-резисторы схемы (как yoRadio PLAYER_FORCE_MONO=false).
    static constexpr bool ampForceMono = false;
    // false — энкодер не крутит громкость (диагностика). true — обычный режим.
    static constexpr bool encoderControlsVolume = true;
    // Старый PCM5102 XSMT — на этой плате не используется.
    static constexpr uint8_t pcm5102XsmtPin = 255;
    static constexpr bool pcm5102XsmtActiveHigh = true;
    // CD4052 / BK8000L на этой плате нет.
    static constexpr uint8_t audioMuxSelectPin = 255;
    static constexpr bool audioMuxWifiIsGpioLow = true;
    static constexpr uint16_t audioMuxSettleUs = 50;
    static constexpr bool bluetoothEnable = false;
    static constexpr uint8_t bk8000UartTxPin = 255;
    static constexpr uint8_t bk8000UartRxPin = 255;
    static constexpr uint32_t bk8000UartBaud = 9600;
    static constexpr uint32_t bk8000UartBootMs = 400;
    static constexpr uint32_t bk8000ReconnectIntervalMs = 12000;
    static constexpr uint8_t bk8000ReconnectMaxAttempts = 3;
    static constexpr bool debugBtUartSerial = false;
    static constexpr uint8_t btModulePowerEnablePin = 255;
    static constexpr bool btModulePowerActiveHigh = true;
    static constexpr uint16_t btModulePowerOnDelayMs = 150;
    static constexpr bool btModuleAtPowerDownOnWifi = false;
    static constexpr const char* btSinkName = "Bender Station";
    static constexpr uint32_t btReconnectFirstDelayMs = 1200;
    static constexpr uint32_t btReconnectRetryMs = 2500;
    static constexpr uint8_t btReconnectBurstCount = 16;
    static constexpr uint32_t btA2dpLastConnPreStackDelayMs = 600;
    static constexpr bool btShowPairingSearchEyes = false;
    // Визуализация рта/EQ по PCM с BK8000L (pcm_analyzer_on_bt_pcm_bytes): отдельные пороги от Wi‑Fi.
    static constexpr uint16_t btPcmAnalyzerGainPercent = 145;
    // Аналог pcmSilenceAbs, но для сырого A2DP PCM (обычно ниже, чем у Wi‑Fi декодера).
    static constexpr uint32_t btPcmSilenceAbs = 220;
    // В core0 noise gate: для BT сравниваем g_pcm_level_adc с data.trsh * percent / 100 (меньше % — раньше «открывается» рот).
    static constexpr uint8_t btPcmNoiseGateTrshPercent = 50;

    // MAX7219×5: CLK / DIN / CS как в bender.cpp (15=CS, 17=DIN, 16=CLK)
    static constexpr uint8_t mtrxClk = 16;
    static constexpr uint8_t mtrxCs = 15;
    static constexpr uint8_t mtrxDat = 17;
    static constexpr uint8_t matrixModuleCount = 5;
    static constexpr uint8_t matrixBrightnessIdleBase = 8;
    // Пока играет радио — яркость матриц не выше этого (общий 5 V с MAX98357; иначе «хрип при тихом»).
    static constexpr uint8_t matrixBrightnessWhenPlayingCap = 4;
    // Период кадра рта/EQ (мс). Было ~42; 56–80 снижает нагрузку SPI MAX7219.
    static constexpr uint16_t matrixVizRefreshMs = 56;
    // Під TTS рот малюємо частіше за радіо; 16 мс — як радіо-колбек по часу кадру.
    static constexpr uint16_t matrixVizTtsRefreshMs = 16;
    // Пауза в мові: vis 0 стільки мс → спокійний кадр. Відкриття одразу, без порога 18.
    static constexpr uint16_t benderMouthRestAfterQuietMs = 60;
    static constexpr uint16_t matrixEyeRefreshMs = 150;
    // 0 = без лимита SPI (на плату лучше без throttle для стабильности кадра).
    static constexpr uint16_t matrixUpdateMinIntervalMs = 0;
    // Калибровка модулей как абсолютный offset от минимального (0..15).
    // Итоговая яркость каждого модуля = base + offset[i], где base двигается энкодером.
    static constexpr int8_t matrixBrightnessTrimMin = 0;
    static constexpr int8_t matrixBrightnessTrimMax = 15;

    // Энкодер: ENC_BTNR / ENC_BTNL / ENC_BTNB из myoptions.h
    static constexpr uint8_t encS1 = 4;
    static constexpr uint8_t encS2 = 5;
    static constexpr uint8_t encBtn = 6;
    // 5 кліків — % АКБ; 6 — Pong; 7 — deep sleep; 8 — restart.
    // Утримання без кліків/повороту (~0.5 с) підтверджує PTT (не клік). Мікрофон пише з моменту натискання.
    static constexpr uint16_t encoderPttHoldMs = 520;
    static constexpr uint8_t encoderSleepClicks = 7;
    static constexpr uint8_t encoderRestartClicks = 8;
    // Без музики і без розмови з Бендером — очі/рот у спокійний режим (не deep sleep).
    static constexpr uint32_t benderFaceCalmAfterMs = 5ul * 60ul * 1000ul;
    // Далі бездіяльність (немає музики, PTT, енкодера, WebUI) — deep sleep, будить кнопка.
    static constexpr uint32_t benderIdleDeepSleepMs = 30ul * 60ul * 1000ul;
    // Застарілі пороги hold-sleep/reset (сон і restart тепер кліками).
    static constexpr uint16_t encoderSleepHoldMs = 5000;
    static constexpr uint16_t encoderHardResetHoldMs = 10000;
    static constexpr uint16_t btForgetPairedHoldMs = 1400;
    // Wi‑Fi: 4×клик + удержание без поворота — вкл/выкл SoftAP (как по духу жесту сброса BT).
    static constexpr uint16_t encoderSoftApToggleHoldMs = 2000;

    // Раньше: АЦП для VolAnalyzer. Сейчас уровень берётся из PCM в audio_process_extern (см. BendeRadio.ino).
    static constexpr uint8_t analyzPin = 34;

    // АКБ 2S Li-ion через делитель на ADC1 (ESP32-S3: GPIO 1…10).
    // На S3-N16R8 GPIO 32/33 заняты Octal Flash/PSRAM — не использовать.
    // Ubat —[Rверх 100k]— вузол —[Rниз 47k]— GND; ratio = U_пакета / U_на_ADC.
    // Номинал (100+47)/47 ≈ 3.13; калибровка по мультиметру: 8.20 V / 2.468 V ≈ 3.32.
    static constexpr bool batteryMonitorEnable = true;
    static constexpr uint8_t batteryAdcPin = 1;
    static constexpr float batteryDividerRatio = 8200.0f / 2468.0f;
    // % з напруги: ступінчаста таблиця U→% у battery.cpp (без інтерполяції між точками).
    // Пороги для battery_eye_mood() (якщо підключиш настрій очей за АКБ).
    static constexpr uint8_t batteryMoodCheerfulMinPct = 70;
    static constexpr uint8_t batteryMoodNormalMinPct = 30;
    static constexpr uint32_t batterySampleIntervalMs = 180000;  // 3 мин
    // Ниже этого % опрашиваем АКБ чаще (batteryLowSampleIntervalMs) — для порога выключения и стабильности.
    static constexpr uint8_t batteryLowAttentionPercent = 10;
    static constexpr uint32_t batteryLowSampleIntervalMs = 60000;  // 1 мин при < batteryLowAttentionPercent
    // Ниже порога без зарядки: глубокий сон (только если batteryMonitorEnable и chargingDetectEnable).
    static constexpr bool batteryShutdownEnable = true;
    static constexpr uint8_t batteryShutdownBelowPercent = 5;
    // Подряд столько замеров (с интервалом выше) должны быть < порога — защита от шума АЦП.
    static constexpr uint8_t batteryShutdownConsecutiveSamples = 2;
    // Ниже этого % (без активной зарядки) — грустные глаза по битмапу (левый/правый — отдельные массивы в core0).
    static constexpr bool batterySadEyesEnable = true;
    static constexpr uint8_t batterySadEyesBelowPercent = 20;
    // Кадр 0…7 (индекс в IMAGES левого/правого глаза).
    static constexpr uint8_t batterySadEyesBitmapIndex = 5;
    // Зрачок 2×2 на грустных глазах: шаг анимации (мс).
    static constexpr uint16_t batterySadEyesPupilStepMs = 1200;
    // Грустные глаза: зрачок позиционируется от центра (dx,dy) в core0; сдвиг не используется.
    static constexpr int8_t batterySadEyesPupilOffsetY = 0;
    // 5 кликов: % АКБ на «роте»; скільки мс показувати (потім зникає).
    static constexpr uint32_t batteryPercentShowDurationMs = 3000;
    // Пока идёт зарядка — дольше держим иконку (анимация заливки).
    static constexpr uint32_t batteryPercentShowDurationChargingMs = 12000;
    // Детект зарядки IP2326: линия CHG/LED через делитель → GPIO12 (ADC2_CH1).
    // НЕ GPIO19/20 — USB D+/D− (убьёт Serial). Без провода на пине — INPUT_PULLDOWN в battery.cpp.
    static constexpr bool chargingDetectEnable = true;
    static constexpr uint8_t chargingDetectPin = 12;
    static constexpr bool chargingDetectUseAdc = true;
    static constexpr uint16_t chargingDetectMinMv = 1000;  // >1.0 V = идёт зарядка
    static constexpr bool chargingDetectActiveHigh = true;   // только для digital-режима
    static constexpr uint32_t chargingDebugSerialMs = 15000;
    // Крок анімації заливки батареї (мс на кадр; кадр = лічильник для battery_matrix_rows_charging).
    static constexpr uint16_t batteryChargeIconAnimStepMs = 420;
    // Повна заливка внутрішньої зони 8×8 батареї лише при pct > цього (див. battery_matrix.cpp).
    static constexpr uint8_t batteryMatrixFullMinPct = 95;
    // true — віддзеркалити батарею по вертикалі (ряд 0 ↔ 7).
    static constexpr bool batteryMatrixInvertY = false;

    // INMP441 (I2S1, отдельная шина от MAX98357). Справочник для будущего AI/записи — в прошивке не используется.
    // VDD=3.3V, GND и L/R=GND, SCK(BCK)=9, WS=10, SD=8.
    static constexpr uint8_t micBclkPin = 9;
    static constexpr uint8_t micWsPin = 10;
    static constexpr uint8_t micDinPin = 8;
    static constexpr uint32_t micSampleRate = 16000;
    // INMP441 в 32-bit I2S-слоте: 14 — баланс громкости; 8 клиппит (peak=32768), 16 часто тихо.
    static constexpr uint8_t micPcmShiftRight = 14;

    // SoftAP: см. encoderSoftApToggleHoldMs + жест 4×клик+удерж. в режиме Wi‑Fi.
    // Станція / гучність на роті — фіксований період matrix_tmr (не довше за batteryPercentShowDurationMs після батареї).
    static constexpr uint16_t matrixOverlayDigitsMs = 1000;

    static constexpr int analyzWidth = 3 * 8;
    static constexpr int radioBuffer = 1600 * 28;  // underrun → «захлеб» / скачет громкость

    // Режим 0: синусоида «струна» + FM и шум — хаотичнее, чем одна гладкая sin.
    static constexpr float analyzSinePeriodsAcross = 1.5f;
    // Скорость фазы (рад/кадр): vol=0 → min (чуть выше нуля — волна не замирает на «тишине» визуализации).
    static constexpr float analyzSineOmegaMin = 0.034f;
    static constexpr float analyzSineOmegaMax = 0.42f;
    static constexpr float analyzSineOmegaEase = 0.28f;
    static constexpr float analyzSineAmpMax = 3.2f;
    // Глубина FM: sin(… + fmDepth * sin(φ₂ + k₂·x)); φ₂ крутится быстрее основной фазы.
    static constexpr float analyzWaveFmDepth = 0.65f;
    static constexpr float analyzWaveChaosOmegaRatio = 2.35f;
    static constexpr float analyzWaveChaosK2 = 0.88f;
    // Доля шума Perlin к амплитуде (0 = выкл).
    static constexpr float analyzWaveNoiseMix = 0.28f;
    // Сдвиг центра по Y (строки): −1 — вся «полоска» рота/волни на 1 піксель вгору; +1 — нижче.
    static constexpr int8_t analyzWaveRowOffset = -1;

    // Рот робота (data.mode 3 і 4): параметри губ; див. core0 mouth_robot_*.
    static constexpr uint8_t analyzMouthEdgeCols = 3;
    static constexpr int8_t analyzMouthEdgeUpperRow = 3;
    static constexpr int8_t analyzMouthEdgeLowerRow = 6;
    static constexpr bool analyzMouthAnchorNoBob = true;
    static constexpr uint8_t analyzMouthCurveKind = 0;
    static constexpr float analyzMouthHyperK = 4.2f;
    static constexpr float analyzMouthHalfSepMin = 0.15f;
    static constexpr float analyzMouthHalfSepMax = 2.15f;
    static constexpr float analyzMouthPhiOmegaMin = 0.055f;
    static constexpr float analyzMouthPhiOmegaMax = 0.26f;
    static constexpr float analyzMouthBobAmp = 0.28f;
    static constexpr float analyzMouthPhi2OmegaMin = 0.072f;
    static constexpr float analyzMouthPhi2OmegaMax = 0.34f;
    static constexpr float analyzMouthChompHarm = 1.83f;
    static constexpr float analyzMouthSlowOmegaMin = 0.021f;
    static constexpr float analyzMouthSlowOmegaMax = 0.058f;
    static constexpr float analyzMouthOmegaNoiseAmp = 0.26f;
    static constexpr float analyzMouthMaskRipple = 0.11f;
    static constexpr float analyzMouthChompFloor = 0.12f;
    static constexpr uint8_t analyzMouthMinPixelGap = 1;

    // EQ (у прошивці data.mode 2): по колонке на пиксель ширины рта; уровень из буфера + лёгкий разброс EMA.
    static constexpr int pcmEqBandCount = analyzWidth;
    static constexpr uint8_t pcmEqBandSmoothShift = 3;
    // Доп. сглаживание по индексу полосы (см. BendeRadio.ino): было чёт/нечёт, теперь 4 фазы — столбики не в такт.
    static constexpr uint8_t pcmEqBandStaggerSmooth = 1;
    // Лёгкая «игра» высоты по X и времени (0 = выкл): полосы не качаются одним куском.
    static constexpr float pcmEqDecorrelAmount = 0.36f;
    static constexpr float pcmEqDecorrelOmega = 0.085f;
    static constexpr float pcmEqDecorrelColSpread = 0.68f;
    static constexpr float pcmEqDecorrelColSpread2 = 0.41f;
    // Статичная «форма» по колонкам (не двигается во времени): множитель высоты от двух sin(col) — часть линий изначально ниже.
    static constexpr float pcmEqShapeFloor = 0.07f;
    static constexpr float pcmEqShapeK1 = 0.37f;
    static constexpr float pcmEqShapeK2 = 0.59f;
    static constexpr float pcmEqShapeP1 = 0.85f;
    static constexpr float pcmEqShapeP2 = 2.05f;
    // Насколько «впадины» глубокие при t=0: env = floor + span*(deep + (1-deep)*t); 0 = до нуля, 1 = без провалов.
    static constexpr float pcmEqShapeDeep = 0.22f;
    // Нижняя граница t = w1*w2: без этого произведение двух sin даёт почти 0 в центре полоски — «затухание к середине».
    static constexpr float pcmEqShapeTMin = 0.40f;

    // Пока заряжаются входные конденсаторы, 5V может проседать — разнос нагрузки во времени.
    static constexpr uint32_t coldStartBootMs = 250;
    // Пауза перед mtrx.begin(): питание на цепочке MAX7219 (несколько модулей) должно стабилизироваться,
    // иначе часть дисплеев не инициализируется при первом включении. Подберите под свой DC‑DC/линейник.
    static constexpr uint32_t matrixPowerStabilizeBeforeBeginMs = 500;
    // После пробуждения из deep sleep (ext0): короче, чем холодный старт (0 = всегда matrixPowerStabilizeBeforeBeginMs).
    static constexpr uint32_t matrixPowerStabilizeBeforeBeginMsAfterWakeMs = 400;
    // Холодное включение после долгого простоя: повторный begin() и «промывка» регистров MAX7219 (артефакты / нет глаз).
    // 0 = не делать второй begin; 0 циклов = только обычный clear+update.
    static constexpr uint32_t matrixColdBootSecondBeginDelayMs = 100;
    static constexpr uint8_t matrixColdBootFlushCycles = 6;
    static constexpr uint32_t matrixColdBootFlushGapMs = 10;
    // После инициализации MAX7219: не подсвечивать матрицу N мс (прогрев/стабилизация). 0 = сразу показ.
    // После пробуждения из deep sleep обычно 0 — не ждать лишнего.
    static constexpr uint32_t matrixDisplayEnableDelayMs = 0;
    static constexpr uint32_t matrixDisplayEnableDelayMsAfterWakeMs = 0;
    static constexpr uint32_t coldStartMatrixZeroMs = 200;
    static constexpr uint32_t coldStartAfterMatrixMs = 300;
    static constexpr uint32_t coldStartBeforeWifiMs = 400;
    // Пауза перед esp_restart() при смене источника (Wi‑Fi/BT) — снижает артефакты матрицы при быстром переключении.
    static constexpr uint16_t modeSwitchRestartDelayMs = 2000;
    // Количество пользовательских URL интернет-радио, которые можно хранить через Web UI.
    static constexpr uint8_t customStationMaxCount = 16;

    // Перед паузой/сменой станции: сколько мс выводить «тишину» в I2S (gain ramp), пока декодер ещё идёт —
    // иначе DMA останавливается и на усилителе часто слышен ВЧ-писк.
    static constexpr uint32_t audioPauseSilenceRampMs = 450;

    // После connecttohost PCM в буфере бывает раньше звука с динамика — волну не показываем это время.
    static constexpr uint32_t pcmVizStreamSettleMs = 520;

    // Два режима шкалы 0…100 для визуализации (те же единицы, что m_src / m_viz — амплитуда |сэмпл| в буфере):
    // true — адаптивная опора ref: «недавний пик» со спадом (как AGC у эквалайзеров); один тюнинг — pcmAnalyzerRefRelease.
    // false — фиксированный делитель pcmMetricFullScale.
    static constexpr bool pcmUseAdaptiveAnalyzerRef = true;
    // Фиксированная полная шкала, если pcmUseAdaptiveAnalyzerRef == false.
    static constexpr uint32_t pcmMetricFullScale = 26000;
    // Адаптивная опора: не ниже / не выше (стабильность и защита от деления).
    static constexpr uint32_t pcmAnalyzerRefFloor = 5000;
    static constexpr uint32_t pcmAnalyzerRefCeil = 30000;
    // Каждый буфер, если m_viz < ref: ref = max(floor, ref - ref * release / 256). Больше — быстрее спад ref → выше средний inst на матрице.
    static constexpr uint8_t pcmAnalyzerRefRelease = 10;
    // Рост ref к пику: шаг = max(1, (m_viz - ref) >> attackShift). Больше shift — ref не догоняет пики → шкала 0…100 не «сжата» к нулю.
    static constexpr uint8_t pcmAnalyzerRefAttackShift = 3;
    // Ниже — тишина (те же единицы, что m_src).
    static constexpr uint32_t pcmSilenceAbs = 400;
    // TTS Бендера: нижчий поріг, щоб приголосні/атака слова вже відкривали рот.
    static constexpr uint32_t pcmBenderSilenceAbs = 80;
    // Верхняя граница g_pcm_level_adc (BendeRadio.ino: inst * 4095 / 100); порог data.trsh в тех же единицах.
    static constexpr uint16_t pcmLevelAdcMax = 4095;
    // true — выше data.trsh сразу полный g_pcm_vis (без доп. умножения по «пандусу» ADC).
    static constexpr bool pcmNoiseGateBinary = true;
    // Растяжка 0…100 через sqrt: при адаптивном ref мелкий inst → заметнее волна/EQ (0 = выкл).
    static constexpr bool pcmVisSqrtStretch = true;
    // Сглаживание уровня: ema = (ema * ((1<<shift)-1) + target) >> shift.
    static constexpr uint8_t pcmInstSmoothShift = 2;

    // Wi‑Fi в экономичный режим только после wifiIdleSleepAfterMs без активности (не сразу при паузе).
    // Активность: воспроизведение, энкодер, открытие веб‑страницы. Если обрывается поток — выставьте false.
    static constexpr bool wifiSleepWhenSilent = false;
    // 0 = не уводить STA в modem sleep по таймеру.
    static constexpr uint32_t wifiIdleSleepAfterMs = 0;
    // После таймера: true = WIFI_PS_MAX_MODEM (максимум экономии, часто ломает TCP/аудио);
    // false = WIFI_PS_MIN_MODEM (мягче, меньше «системных» сбоев при энергосбережении).
    static constexpr bool wifiIdlePsMaxModem = false;
    // Пауза в конце цикла core0 — уступка CPU и лёгкий idle (0 = выкл.).
    static constexpr uint8_t core0LoopDelayMs = 1;
    // Радио выкл.: реже опрашивать делитель АКБ (0 = всегда batterySampleIntervalMs).
    static constexpr uint32_t batterySampleIntervalIdleMs = 180000;  // 3 мин (радио выкл.)
    // loop(): при выкл. радио — короткая пауза, меньше кручение CPU в ожидании.
    static constexpr uint8_t loopDelayMsWhenRadioOff = 2;
    // После пробуждения из deep sleep (ext0): «бегающие глаза» как при поиске Wi‑Fi (0 = выкл.).
    static constexpr uint32_t wakeAfterSleepAnimMs = 3500;

    // Отладка PCM в audio_process_extern: редкие строки в Serial (не на каждый буфер — иначе глотает аудио/Wi‑Fi).
    static constexpr bool debugAudioPcmSerial = false;
    static constexpr uint32_t debugAudioPcmSerialMs = 300;
};
