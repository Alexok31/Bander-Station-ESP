#pragma once

#include <Arduino.h>
#include "BenderFace.h"

// Бендер realtime (локальний WS / xAI / OpenAI) всередині радіо.
// Утримання кнопки енкодера — PTT; I2S0 на час відповіді забирається в PCM.

void bender_ai_begin();
void bender_ai_tick();
void bender_ai_wake();
void bender_ai_sleep();
bool bender_ai_awake();

bool bender_ai_busy();
bool bender_ai_recording();
bool bender_ai_owns_speaker();
bool bender_ai_tts_playing();
BenderFaceState bender_ai_face_state();

// Чисте утримання (без кліків і повороту). EncButton тримає той самий GPIO6.
void bender_ai_ptt_arm();
void bender_ai_ptt_down();
void bender_ai_ptt_up();
void bender_ai_ptt_cancel();
void bender_ai_yield_radio();

void bender_ai_set_debug(bool on);
bool bender_ai_debug();
