#pragma once

#include <Arduino.h>

// Бендер realtime (локальний WS / xAI / OpenAI) всередині радіо.
// Утримання кнопки енкодера — PTT; I2S0 на час відповіді забирається в PCM.

void bender_ai_begin();
void bender_ai_tick();

bool bender_ai_busy();
bool bender_ai_recording();
bool bender_ai_owns_speaker();
bool bender_ai_tts_playing();

// Чисте утримання (без кліків і повороту). EncButton тримає той самий GPIO6.
void bender_ai_ptt_arm();
void bender_ai_ptt_down();
void bender_ai_ptt_up();
void bender_ai_ptt_cancel();
