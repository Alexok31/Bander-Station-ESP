#pragma once
#include <Arduino.h>
#include <Audio.h>
#include <cstring>

#include "RadioConfig.h"
#include "pcm_analyzer.h"

struct Data {
    bool state = 0;
    int8_t vol = 10;
    int8_t bright_eyes = 5;
    int8_t bright_mouth = 5;
    // Порог «тишины» для PCM-метра (после смены с АЦП сделайте 3 клика на тихой паузе).
    uint16_t trsh = 24;
    // 0 волна; 1 волна інверсія; 2 EQ; 3 рот; 4 рот інверсія; 5 прогресс трека (низ рта, BT AVRCP). Інше → 0.
    uint8_t mode = 0;
    int8_t station = 0;
};

// Не называть глобал data — на ESP32 Arduino 3.x конфликт с std::data.
extern Data radioState;
extern Audio audio;
extern const char* reconnect;
extern volatile bool wifiConnecting;

void change_state();
// I2S/буфер — как можно раньше в setup(), до connecttohost (конструктор Audio уже поднял I2S).
void audio_hw_init(bool log_serial = false);
void apply_output_volume();
void matrix_show_volume(int8_t vol);
void amp_force_mute();
// ESP32-S3: UART0 по умолчанию на 43/44, у нас BCLK=44 — без этого при USB динамик молчит.
void free_uart0_from_i2s_pins();
const char* station_url_for_current();
void anim_search();
void core0(void *p);
void syncWifiWithAudioSilence();
void wifi_touch_activity();
void wifi_ap_toggle_from_core0();
void matrix_get_brightness_trim(int8_t* outTrim, uint8_t count);
void matrix_set_brightness_trim(const int8_t* trim, uint8_t count, bool persist);
uint8_t matrix_get_base_brightness();

uint8_t radio_station_count();
const char* radio_station_name(uint8_t idx);
// Голосова команда з ПК: лише radioState (I2S ще може бути в Бендера).
void radio_voice_set_state(bool on, int station /* -1 = не міняти */);
// Після віддачі I2S радіо: BT play/pause, морда, гучність.
void radio_voice_after_speaker();

// Источник звука: "wifi" (интернет-радио / PCM5100) или "bt" (BK8000L через CD4052). NVS "bende"/"aud".
extern char g_audio_source[8];
extern char g_play_mode[8];
inline bool play_mode_is_airplay() {
    return strcmp(g_play_mode, "ap") == 0;
}
// enum + CD4052: AudioMux.h
// true только после esp_restart() из commitSourceModeSwitch — короткие задержки вместо холодного старта.
extern bool g_warm_boot_after_mode_switch;
void commitSourceModeSwitch(const char* new_mode);
void commitPlayModeSwitch(const char* mode);
void matrix_show_play_mode();