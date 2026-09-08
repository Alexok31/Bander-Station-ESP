#pragma once

#include <Arduino.h>

// AirPlay 1 (RAOP) приймач. iPhone бачить колонку в Control Center.
// На RECORD забираємо I2S0 в радіо; на TEARDOWN / PTT — віддаємо назад.

void airplay_begin();
void airplay_tick();
void airplay_interrupt();

bool airplay_owns_speaker();
bool airplay_playing();
bool airplay_accepts();
void airplay_set_accept(bool on);
void airplay_encoder_vol_changed();
void airplay_dacp_command(const char* cmd);

uint32_t airplay_track_duration_ms();
uint32_t airplay_track_position_ms();
uint32_t airplay_track_meta_serial();
const char* airplay_track_scroll_cstr();
