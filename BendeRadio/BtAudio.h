#pragma once



#include <Arduino.h>



// ESP32-S3: без Bluetooth Classic / ESP32-A2DP.
// Аналог BK8000L -> CD4052 X1/Y1; переключение с PCM5100 — AudioMux (GPIO35). UART — в BtAudio.cpp.

// Жесты, матрица и commitSourceModeSwitch(wifi|bt) в core0/BendeRadio.ino не трогаем.



void bt_audio_start_sink();

void bt_audio_stop_sink();

// Wi‑Fi: полностью глушим BK8000L (AT+CD/CP, UART, опц. GPIO питания). На ESP нет A2DP/BLE.
void bt_audio_shutdown_for_wifi_mode();

bool bt_audio_is_sink_running();

// Sink включён, но телефон ещё не подключён — «бегающие глаза».

bool bt_audio_needs_pairing_ui();

// Сброс сопряжений / режим поиска (без reboot ESP).

void bt_audio_forget_paired_devices();

void bt_audio_tick();



void bt_audio_volume_apply(bool audio_on, int8_t vol_ui);



void bt_audio_avrcp_pause();

void bt_audio_avrcp_play();

void bt_audio_avrcp_next();

void bt_audio_avrcp_previous();



uint32_t bt_audio_track_duration_ms();

uint32_t bt_audio_track_position_ms();

void bt_audio_poll_track_position();



uint32_t bt_audio_track_meta_serial();

const char* bt_audio_track_scroll_cstr();



// 0 — нет; 1 — пауза с телефона; 2 — play. Забирает core0.

uint8_t bt_audio_take_remote_ui_request();



// --- Вызовы для будущего UART-драйвера BK8000L ---

void bt_audio_set_link_connected(bool connected);

void bt_audio_set_playing(bool playing);

void bt_audio_set_track_meta(const char* title, const char* artist);

void bt_audio_set_track_times_ms(uint32_t position_ms, uint32_t duration_ms);

void bt_audio_queue_remote_ui(uint8_t req);


