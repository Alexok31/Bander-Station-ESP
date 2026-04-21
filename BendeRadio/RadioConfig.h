#pragma once

#include <cstdint>

// Все настройки пинов, Wi‑Fi и таймингов «холодного» старта в одном месте.
class RadioConfig {
   public:
    // Значения по умолчанию, если в NVS ещё ничего не сохраняли (первый запуск / сброс).
    static constexpr const char* wifiSsid = "Space Lovers";
    static constexpr const char* wifiPass = "cassini31";

    // Точка доступа ESP для настройки: подключитесь к ней и откройте http://192.168.4.1
    static constexpr const char* apSsid = "BendeRadio";
    // Пароль AP не короче 8 символов (WPA2). Пустая строка = открытая сеть (только для отладки).
    static constexpr const char* apPassDefault = "benderadio";

    static constexpr uint8_t i2sDout = 25;
    static constexpr uint8_t i2sBclk = 27;
    static constexpr uint8_t i2sLrc = 26;
    // Имя Bluetooth A2DP sink (телефон увидит это в списке сопряжения).
    static constexpr const char* btSinkName = "Bender";
    // Автоподключение к последнему телефону: пауза перед первым reconnect из loop; интервал повторов.
    static constexpr uint32_t btReconnectFirstDelayMs = 1200;
    static constexpr uint32_t btReconnectRetryMs = 2500;
    static constexpr uint8_t btReconnectBurstCount = 16;
    // Пауза внутри ESP32-A2DP при наличии last_bda в NVS (до init Bluetooth) — больше = спокойнее после подачи питания.
    static constexpr uint32_t btA2dpLastConnPreStackDelayMs = 600;
    // Визуализация рта/EQ по BT: в библиотеке set_stream_reader вызывается ПОСЛЕ цифровой громкости A2DP — сигнал часто «ниже» Wi‑Fi.
    // Берём raw_stream_reader (до volume) + отдельные пороги. gain 100 = без усиления; 120…200 — если всё ещё бідно.
    static constexpr uint16_t btPcmAnalyzerGainPercent = 145;
    // Аналог pcmSilenceAbs, но для сырого A2DP PCM (обычно ниже, чем у Wi‑Fi декодера).
    static constexpr uint32_t btPcmSilenceAbs = 220;
    // В core0 noise gate: для BT сравниваем g_pcm_level_adc с data.trsh * percent / 100 (меньше % — раньше «открывается» рот).
    static constexpr uint8_t btPcmNoiseGateTrshPercent = 50;

    static constexpr uint8_t mtrxCs = 22;
    static constexpr uint8_t mtrxDat = 23;
    static constexpr uint8_t mtrxClk = 21;

    static constexpr uint8_t encS1 = 19;
    static constexpr uint8_t encS2 = 18;
    static constexpr uint8_t encBtn = 4;
    // Жесты (EncButton): удерж.+поворот — Wi‑Fi: станция; BT: AVRCP next/prev; двойной+поворот — режим рта (6 вар.); 1 клик — пуск/пауза; 3 тапа — порог; 4×клик+удерж.+поворот — wfi/bt; 4×клик+удерж. без поворота (BT) — сброс сопряжений; 5 — АКБ; 6 — Pong; 7 — SoftAP.
    // Кнопка энкодера (GPIO 4 = RTC): отпустить после 5–9 с удержания — deep sleep (если за удержание не было поворота с нажатой кнопкой);
    // держать ≥10 с без отпускания — ESP.restart() (то же: при удерж.+повороте станция/яркость/громкость — не срабатывает).
    static constexpr uint16_t encoderSleepHoldMs = 5000;
    static constexpr uint16_t encoderHardResetHoldMs = 10000;
    static constexpr uint16_t btForgetPairedHoldMs = 1400;

    // Раньше: АЦП для VolAnalyzer. Сейчас уровень берётся из PCM в audio_process_extern (см. BendeRadio.ino).
    static constexpr uint8_t analyzPin = 34;

    // АКБ 2S Li-ion через делитель на ADC1 (тільки input-only), напр. GPIO 35:
    // Ubat —[Rверх 100k]— вузол —[Rниз 47k]— GND; ratio = (100+47)/47.
    // Якщо у тебе навпаки (47k до батареї, 100k до землі), постав ratio = (47+100)/100.
    static constexpr bool batteryMonitorEnable = true;
    static constexpr uint8_t batteryAdcPin = 35;
    static constexpr float batteryDividerRatio = (100.0f + 47.0f) / 47.0f;
    // Калібрування % під мультиметр на клемах пакета (після BMS).
    static constexpr uint16_t batteryEmptyMv = 6200;
    static constexpr uint16_t batteryFullMv = 8300;
    // Пороги для battery_eye_mood() (якщо підключиш настрій очей за АКБ).
    static constexpr uint8_t batteryMoodCheerfulMinPct = 70;
    static constexpr uint8_t batteryMoodNormalMinPct = 30;
    static constexpr uint32_t batterySampleIntervalMs = 180000;  // 3 мин
    // 4 кліки: % АКБ на «роті»; скільки мс показувати (потім зникає).
    static constexpr uint32_t batteryPercentShowDurationMs = 3000;
    // Линия индикации зарядки IP2326 (через делитель 68k/100k): HIGH ≈ идёт зарядка, LOW ≈ завершена.
    static constexpr bool chargingDetectEnable = true;
    static constexpr uint8_t chargingDetectPin = 33;
    // Крок анімації заливки батареї (мс на кадр; кадр = лічильник для battery_matrix_rows_charging).
    static constexpr uint16_t batteryChargeIconAnimStepMs = 420;
    // Повна заливка внутрішньої зони 8×8 батареї лише при pct > цього (див. battery_matrix.cpp).
    static constexpr uint8_t batteryMatrixFullMinPct = 95;
    // true — віддзеркалити батарею по вертикалі (ряд 0 ↔ 7).
    static constexpr bool batteryMatrixInvertY = false;
    // Під час зарядки напруга на пакеті завищена — % з напруги «стрибає» вгору. Якщо true: від якоря
    // (останній % у момент появи зарядки) показ не росте швидше за N відсотків на хвилину.
    static constexpr bool batteryChargeSocCapEnable = true;
    static constexpr uint8_t batteryChargeSocMaxRisePerMinute = 4;
    // 6 кликов: вкл/выкл точку доступа (SoftAP) для веб-настройки, если STA уже подключён; без STA при работающем AP не отключается.
    // Станція / гучність на роті — фіксований період matrix_tmr (не довше за batteryPercentShowDurationMs після батареї).
    static constexpr uint16_t matrixOverlayDigitsMs = 1000;

    static constexpr int analyzWidth = 3 * 8;
    static constexpr int radioBuffer = 1600 * 25;  // default 1600*5 — мало для потока

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

    // Рот робота (у прошивці data.mode 3 і 4): параметри губ; див. core0 mouth_robot_*.
    // Перші та останні analyzMouthEdgeCols колонок: верх/низ завжди на цих рядках (+ analyzWaveRowOffset).
    static constexpr uint8_t analyzMouthEdgeCols = 3;
    static constexpr int8_t analyzMouthEdgeUpperRow = 3;
    static constexpr int8_t analyzMouthEdgeLowerRow = 6;
    // true — без вертикального bob (інакше краї «відриваються» візуально від якорів).
    static constexpr bool analyzMouthAnchorNoBob = true;
    // Форма «звідності» відкриття тільки по середині: 0 — парабола (1−t²), 1 — гіпербола 1/(1+k·t²), t по внутрішній ширині.
    static constexpr uint8_t analyzMouthCurveKind = 0;
    static constexpr float analyzMouthHyperK = 4.2f;
    // Додаткове розкриття в центрі (напів-інтервал у float), множиться на open_mask і chomp·vol.
    static constexpr float analyzMouthHalfSepMin = 0.15f;
    static constexpr float analyzMouthHalfSepMax = 2.15f;
    // Кивок усього рта (вимикається, якщо analyzMouthAnchorNoBob).
    static constexpr float analyzMouthPhiOmegaMin = 0.055f;
    static constexpr float analyzMouthPhiOmegaMax = 0.26f;
    static constexpr float analyzMouthBobAmp = 0.28f;
    // φ₂ — «челюсть»; друга гармоніка + повільна фаза + шум кроку — менш рівний ритм.
    static constexpr float analyzMouthPhi2OmegaMin = 0.072f;
    static constexpr float analyzMouthPhi2OmegaMax = 0.34f;
    static constexpr float analyzMouthChompHarm = 1.83f;
    static constexpr float analyzMouthSlowOmegaMin = 0.021f;
    static constexpr float analyzMouthSlowOmegaMax = 0.058f;
    static constexpr float analyzMouthOmegaNoiseAmp = 0.26f;
    // Дрібна хвиля по open_mask у середині (0 = вимк.); краї якорів не чіпає (×0 там).
    static constexpr float analyzMouthMaskRipple = 0.11f;
    // Нижняя граница множителя chomp при sin=-1 (узкий зев, но см. analyzMouthMinPixelGap).
    static constexpr float analyzMouthChompFloor = 0.12f;
    // Мінімальний відступ між верхньою і нижньою лініями (рядки матриці).
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
    static constexpr uint32_t matrixPowerStabilizeBeforeBeginMs = 2000;
    // После пробуждения из deep sleep (ext0): короче, чем холодный старт (0 = всегда matrixPowerStabilizeBeforeBeginMs).
    static constexpr uint32_t matrixPowerStabilizeBeforeBeginMsAfterWakeMs = 400;
    // Холодное включение после долгого простоя: повторный begin() и «промывка» регистров MAX7219 (артефакты / нет глаз).
    // 0 = не делать второй begin; 0 циклов = только обычный clear+update.
    static constexpr uint32_t matrixColdBootSecondBeginDelayMs = 100;
    static constexpr uint8_t matrixColdBootFlushCycles = 6;
    static constexpr uint32_t matrixColdBootFlushGapMs = 10;
    // После инициализации MAX7219: не подсвечивать матрицу N мс (прогрев/стабилизация). 0 = сразу показ.
    // После пробуждения из deep sleep обычно 0 — не ждать лишнего.
    static constexpr uint32_t matrixDisplayEnableDelayMs = 2000;
    static constexpr uint32_t matrixDisplayEnableDelayMsAfterWakeMs = 0;
    static constexpr uint32_t coldStartMatrixZeroMs = 200;
    static constexpr uint32_t coldStartAfterMatrixMs = 300;
    static constexpr uint32_t coldStartBeforeWifiMs = 400;
    // Пауза перед esp_restart() при смене источника (Wi‑Fi/BT) — снижает артефакты матрицы при быстром переключении.
    static constexpr uint16_t modeSwitchRestartDelayMs = 2000;

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
    static constexpr bool wifiSleepWhenSilent = true;
    // 0 = не переводить Wi‑Fi в сон по таймеру. Иначе мс бездействия до WiFi.setSleep + WIFI_PS_MAX_MODEM.
    // Тест: 10 с; в бою поставьте обратно 300000 (5 мин).
    static constexpr uint32_t wifiIdleSleepAfterMs = 10000;
    // После таймера бездействия — MAX_MODEM; иначе NONE (без задержки при возобновлении стрима).
    static constexpr bool wifiPsMaxModemWhenSilent = true;
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
