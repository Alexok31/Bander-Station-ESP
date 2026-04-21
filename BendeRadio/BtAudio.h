#pragma once

#include <Arduino.h>

// Требуется библиотека «ESP32-A2DP» (Phil Schätzmann) в Arduino Library Manager.
void bt_audio_start_sink();
void bt_audio_stop_sink();
bool bt_audio_is_sink_running();
// Sink запущен, но A2DP ещё не подключён — можно показывать ожидание (бегающие глаза).
bool bt_audio_needs_pairing_ui();
// Сбросить сохранённые BT-сопряжения и заново запустить sink в режиме поиска нового устройства (без reboot).
void bt_audio_forget_paired_devices();
// Периодически вызывать из loop в режиме BT: отложенные попытки reconnect к последнему устройству.
void bt_audio_tick();

// Громкость A2DP (0…127 внутри библиотеки): vol_ui как у радио 0…21, audio_on как data.state.
void bt_audio_volume_apply(bool audio_on, int8_t vol_ui);

// AVRCP только на телефон: пауза/плей (без чтения состояния с телефона).
void bt_audio_avrcp_pause();
void bt_audio_avrcp_play();
void bt_audio_avrcp_next();
void bt_audio_avrcp_previous();

// Для режима рта «прогресс трека» (AVRCP): длительность из метаданных, позиция из RN (не все телефоны шлют).
uint32_t bt_audio_track_duration_ms();
uint32_t bt_audio_track_position_ms();
// Повторно зарегистрировать AVRCP PLAY_POS_CHANGED — часто приходит interim с текущей позицией (мс).
void bt_audio_poll_track_position();

// Метаданные для бегущей строки (UTF-8). serial меняется при смене title/artist или трека — сброс прокрутки.
uint32_t bt_audio_track_meta_serial();
const char* bt_audio_track_scroll_cstr();

// Запрос из AVRCP: 0 — нет, 1 — как пауза на энкодере (спящие глаза), 2 — как плей. Забрать в task core0.
uint8_t bt_audio_take_remote_ui_request();
