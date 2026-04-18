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

// AVRCP: телефон сообщил новый статус воспроизведения — записать в *new_playing и вернуть true.
bool bt_audio_consume_remote_playstate(bool* new_playing);
// Отправить play/pause на телефон (если AVRCP подключён).
void bt_audio_avrcp_play();
void bt_audio_avrcp_pause();
bool bt_audio_is_avrc_connected();
