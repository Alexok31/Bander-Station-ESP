#pragma once

#include <Arduino.h>

// AirPlay 1 (RAOP) приймач. iPhone бачить колонку в Control Center.
// На RECORD забираємо I2S0 в радіо; на TEARDOWN / PTT — віддаємо назад.

void airplay_begin();
void airplay_tick();
void airplay_interrupt();

bool airplay_owns_speaker();
bool airplay_playing();
void airplay_encoder_vol_changed();
