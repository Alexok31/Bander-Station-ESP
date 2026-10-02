#pragma once

#include <Arduino.h>
#include "BenderFace.h"
#include "BenderLife.h"
#include "CharacterSettings.h"
#include "CharacterPreview.h"

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
bool bender_ai_favorite_station(int station);
bool bender_ai_event_voice_enabled();
// Best effort: stale/busy/offline events are dropped, never queued for later.
bool bender_ai_event(BenderEvent event, int station);
// Preview never persists traits. Status: 0 idle, 1 connecting, 2 generating,
// 3 draining playback, 4 done, 5 failed/cancelled. Called from WebUI/core0.
const char* bender_ai_preview_character(const BenderCharacter::Settings& settings, const char* question);
uint8_t bender_ai_preview_status();

// Чисте утримання (без кліків і повороту). EncButton тримає той самий GPIO6.
void bender_ai_ptt_arm();
void bender_ai_ptt_down();
void bender_ai_ptt_up();
void bender_ai_ptt_cancel();
void bender_ai_yield_radio();

void bender_ai_set_debug(bool on);
bool bender_ai_debug();
