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

    static constexpr uint8_t mtrxCs = 22;
    static constexpr uint8_t mtrxDat = 23;
    static constexpr uint8_t mtrxClk = 21;

    static constexpr uint8_t encS1 = 19;
    static constexpr uint8_t encS2 = 18;
    static constexpr uint8_t encBtn = 5;

    // Раньше: АЦП для VolAnalyzer. Сейчас уровень берётся из PCM в audio_process_extern (см. BendeRadio.ino).
    static constexpr uint8_t analyzPin = 34;

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
    static constexpr uint32_t coldStartMatrixZeroMs = 200;
    static constexpr uint32_t coldStartAfterMatrixMs = 300;
    static constexpr uint32_t coldStartBeforeWifiMs = 400;

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

    // При vol==0 или выкл. радио включает сон модема Wi‑Fi — иногда слабее слышен ВЧ-писк в усилителе.
    // Если обрывается поток — выставьте false.
    static constexpr bool wifiSleepWhenSilent = true;

    // Отладка PCM в audio_process_extern: редкие строки в Serial (не на каждый буфер — иначе глотает аудио/Wi‑Fi).
    static constexpr bool debugAudioPcmSerial = false;
    static constexpr uint32_t debugAudioPcmSerialMs = 300;
};
