#pragma once
#include <Arduino.h>
#include <Audio.h>

#include "RadioConfig.h"

struct Data {
    bool state = 0;
    int8_t vol = 10;
    int8_t bright_eyes = 5;
    int8_t bright_mouth = 2;
    // Порог «тишины» для PCM-метра (после смены с АЦП сделайте 3 клика на тихой паузе).
    uint16_t trsh = 24;
    // 0 — «волна» (Perlin), 2 — бегущая PCM-волна (столбцы, симметрия от центра); значение 1 в EEPROM → 0.
    uint8_t mode = 0;
    int8_t station = 0;
};

extern Audio audio;
extern const char* reconnect;
extern volatile bool wifiConnecting;
// Уровень из PCM (audio_process_extern): мгновенная шкала для порога и 0…100 для эквалайзера.
extern volatile uint16_t g_pcm_level_adc;
extern volatile uint8_t g_pcm_vis;
extern volatile uint8_t g_pcm_vis_inst;
extern volatile int8_t g_pcm_bend_exc;
// Режим 2: бегущая волна — массив сдвигается на core0, справа подмешивается latest из PCM-колбэка.
extern volatile uint8_t g_pcm_wave_amp[RadioConfig::pcmWaveBarCount];
extern volatile uint8_t g_pcm_wave_latest_half;

void change_state();
void anim_search();
void core0(void *p);
void syncWifiWithAudioSilence();