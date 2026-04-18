#pragma once

#include <Arduino.h>

// Требуется библиотека «ESP32-A2DP» (Phil Schätzmann) в Arduino Library Manager.
void bt_audio_start_sink();
void bt_audio_stop_sink();
bool bt_audio_is_sink_running();
// Sink запущен, но A2DP ещё не подключён — можно показывать ожидание (бегающие глаза).
bool bt_audio_needs_pairing_ui();
