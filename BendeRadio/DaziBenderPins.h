#pragma once

// Пины Bender V3 для примеров DAZI-AI (см. RadioConfig.h).
// MAX98357A — та же шина I2S, что и интернет-радио.
#define DAZI_I2S_DOUT 21
#define DAZI_I2S_BCLK 44
#define DAZI_I2S_LRC 2

// INMP441 — отдельная шина I2S (не 4/5/6: там энкодер).
#define DAZI_MIC_BCLK 9
#define DAZI_MIC_WS 10
#define DAZI_MIC_DIN 8

// Кнопка энкодера (активный LOW). В примерах DAZI — GPIO0 BOOT.
#define DAZI_TALK_BUTTON_PIN 6

#define DAZI_SAMPLE_RATE 16000
