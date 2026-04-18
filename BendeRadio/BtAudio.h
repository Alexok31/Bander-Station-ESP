#pragma once

#include <Arduino.h>

// Требуется библиотека «ESP32-A2DP» (Phil Schätzmann) в Arduino Library Manager.
void bt_audio_start_sink();
void bt_audio_stop_sink();
bool bt_audio_is_sink_running();
// Sink запущен, но A2DP ещё не подключён — можно показывать ожидание (бегающие глаза).
bool bt_audio_needs_pairing_ui();
// Громкость A2DP (0…127 внутри библиотеки): vol_ui как у радио 0…21, audio_on как data.state.
void bt_audio_volume_apply(bool audio_on, int8_t vol_ui);

// AVRCP только на телефон: пауза/плей (без чтения состояния с телефона).
void bt_audio_avrcp_pause();
void bt_audio_avrcp_play();
