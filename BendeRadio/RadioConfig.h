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

    // Режим 0 / 2: синусоида «струна» — полных периодов на ширину рта, амплитуда и скорость от vol (PCM).
    static constexpr float analyzSinePeriodsAcross = 1.5f;
    // Скорость фазы (рад/кадр матрицы): vol=0 → min, vol=100 → max (чем больше разрыв, тем сильнее «газ» от музыки).
    static constexpr float analyzSineOmegaMin = 0.018f;
    static constexpr float analyzSineOmegaMax = 0.36f;
    // 0.15…1.0 — насколько плавно подстраивается скорость под скачки vol (1 = без сглаживания).
    static constexpr float analyzSineOmegaEase = 0.22f;
    static constexpr float analyzSineAmpMax = 3.2f;
    // Сдвиг центра по Y (строки): +1 — волна ниже.
    static constexpr int8_t analyzWaveRowOffset = 0;

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
    // Каждый буфер, если m_viz < ref: ref = max(floor, ref - ref * release / 256). Больше — быстрее подстраивается под тихие фрагменты.
    static constexpr uint8_t pcmAnalyzerRefRelease = 5;
    // Рост ref к пику не мгновенно: шаг = max(1, (m_viz - ref) >> attackShift). Иначе ref≈m → inst≈const.
    static constexpr uint8_t pcmAnalyzerRefAttackShift = 2;
    // Ниже — тишина (те же единицы, что m_src).
    static constexpr uint32_t pcmSilenceAbs = 400;
    // Сглаживание уровня: ema = (ema * ((1<<shift)-1) + target) >> shift.
    static constexpr uint8_t pcmInstSmoothShift = 2;

    // При vol==0 или выкл. радио включает сон модема Wi‑Fi — иногда слабее слышен ВЧ-писк в усилителе.
    // Если обрывается поток — выставьте false.
    static constexpr bool wifiSleepWhenSilent = true;

    // Отладка PCM в audio_process_extern: редкие строки в Serial (не на каждый буфер — иначе глотает аудио/Wi‑Fi).
    static constexpr bool debugAudioPcmSerial = true;
    static constexpr uint32_t debugAudioPcmSerialMs = 300;
};
