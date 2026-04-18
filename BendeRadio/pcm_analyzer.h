#pragma once

#include <Arduino.h>
#include <stdint.h>

#include "RadioConfig.h"

// Глобали для матрицы (определение в pcm_analyzer.cpp).
extern volatile uint16_t g_pcm_level_adc;
extern volatile uint8_t g_pcm_vis;
extern volatile uint8_t g_pcm_eq_band[RadioConfig::pcmEqBandCount];

void pcm_analyzer_reset();
void pcm_analyzer_begin_stream_settle();

// Колбэк декодера Wi‑Fi: len_frames — число кадров (стерео пары), ch — каналы из audio.getChannels().
void pcm_analyzer_on_decoder_buffer(int16_t* buff, uint16_t len_frames, uint8_t ch, bool stream_running);

// PCM из A2DP (s16le стерео), len_bytes — длина буфера в байтах.
void pcm_analyzer_on_bt_pcm_bytes(const uint8_t* pcm_bytes, uint32_t len_bytes);
