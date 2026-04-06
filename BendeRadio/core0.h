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
    // 0 волна; 1 волна інверсія; 2 EQ; 3 рот; 4 рот інверсія. Інше → 0.
    uint8_t mode = 0;
    int8_t station = 0;
};

extern Audio audio;
extern const char* reconnect;
extern volatile bool wifiConnecting;
// Уровень из PCM (audio_process_extern): шкала 0…100 и «АЦП» для порога тишины на матрице.
extern volatile uint16_t g_pcm_level_adc;
extern volatile uint8_t g_pcm_vis;
extern volatile uint8_t g_pcm_eq_band[RadioConfig::pcmEqBandCount];

void change_state();
void anim_search();
void core0(void *p);
void syncWifiWithAudioSilence();