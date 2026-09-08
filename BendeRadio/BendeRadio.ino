#include <Arduino.h>
#include <cstring>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_sleep.h>
#include <esp_system.h>
#include <esp_wifi.h>
#include "AudioMux.h"
#include "BtAudio.h"
#include "NvsConfig.h"
#include "RadioConfig.h"
#include "WebUi.h"
#include "core0.h"
#include "BenderAi.h"
#include "AirPlay.h"

TaskHandle_t Task0;

char g_audio_source[8] = "wifi";
char g_play_mode[8] = "radio";
bool g_warm_boot_after_mode_switch = false;

void commitSourceModeSwitch(const char* new_mode) {
    if (!RadioConfig::bluetoothEnable) {
        return;
    }
    if (strcmp(new_mode, "wifi") != 0 && strcmp(new_mode, "bt") != 0) {
        return;
    }
    Preferences prefs;
    prefs.begin("bende", false);
    prefs.putString("aud", new_mode);
    prefs.putBool("wmrst", true);
    prefs.end();
    audio_mux_apply_legacy_string(new_mode);
    if (strcmp(new_mode, "wifi") == 0) {
        bt_audio_shutdown_for_wifi_mode();
    }
    delay(RadioConfig::modeSwitchRestartDelayMs);
    esp_restart();
}

void commitPlayModeSwitch(const char* mode) {
    if (!mode) {
        return;
    }
    if (strcmp(mode, "ap") == 0 && !RadioConfig::airplayEnable) {
        return;
    }
    if (strcmp(mode, "radio") != 0 && strcmp(mode, "ap") != 0) {
        return;
    }
    const bool same = strcmp(g_play_mode, mode) == 0;
    strncpy(g_play_mode, mode, sizeof(g_play_mode) - 1);
    g_play_mode[sizeof(g_play_mode) - 1] = '\0';
    Preferences prefs;
    prefs.begin("bende", false);
    prefs.putString("play", g_play_mode);
    prefs.end();
    if (strcmp(mode, "ap") == 0) {
        radioState.state = false;
        if (strcmp(g_audio_source, "wifi") == 0) {
            audio.setVolume(0);
            if (audio.isRunning()) {
                audio.stopSong();
            }
        }
        apply_output_volume();
        airplay_set_accept(true);
    } else {
        airplay_set_accept(false);
    }
    Serial.printf("[Mode] %s\n", strcmp(g_play_mode, "ap") == 0 ? "AIR" : "FM");
    if (!same) {
        change_state();
    }
    matrix_show_play_mode();
}

extern Data radioState;
String g_api_host;

static void radio_pcm_viz(int16_t* buff, uint16_t len, uint8_t ch, bool* continueI2S) {
    *continueI2S = true;
    if (bender_ai_owns_speaker()) {
        return;
    }
    if (strcmp(g_audio_source, "bt") == 0) {
        return;
    }
    if (ch == 0) {
        ch = audio.getChannels();
        if (ch == 0) {
            ch = 2;
        }
    }
    pcm_analyzer_on_decoder_buffer(buff, len, ch, audio.isRunning());
}

// ESP32-audioI2S 2.x (якщо колись знову підключиться).
void audio_process_extern(int16_t* buff, uint16_t len, bool* continueI2S) {
    radio_pcm_viz(buff, len, 0, continueI2S);
}

// ESP32-audioI2S 3.1 (Wolle): рот і EQ під музику.
void audio_process_i2s(int16_t* outBuff, uint16_t validSamples, uint8_t bitsPerSample, uint8_t channels,
                       bool* continueI2S) {
    (void)bitsPerSample;
    radio_pcm_viz(outBuff, validSamples, channels, continueI2S);
}

void setup() {
    // Сначала NVS (до core0 и до холодных delay): режим + флаг «тёплой» перезагрузки после смены Wi‑Fi/BT.
    {
        Preferences prefs;
        prefs.begin("bende", true);
        g_warm_boot_after_mode_switch = prefs.getBool("wmrst", false);
        String s = prefs.getString("aud", "wifi");
        String play = prefs.getString("play", "radio");
        if (!RadioConfig::bluetoothEnable || (s != "wifi" && s != "bt")) {
            s = "wifi";
            prefs.end();
            prefs.begin("bende", false);
            prefs.putString("aud", s);
            prefs.end();
        } else {
            prefs.end();
        }
        if (play != "radio" && play != "ap") {
            play = "radio";
        }
        if (!RadioConfig::airplayEnable) {
            play = "radio";
        }
        strncpy(g_play_mode, play.c_str(), sizeof(g_play_mode) - 1);
        g_play_mode[sizeof(g_play_mode) - 1] = '\0';
        strncpy(g_audio_source, s.c_str(), sizeof(g_audio_source));
        g_audio_source[sizeof(g_audio_source) - 1] = '\0';
        if (g_warm_boot_after_mode_switch) {
            prefs.begin("bende", false);
            prefs.putBool("wmrst", false);
            prefs.end();
        }
    }
    strncpy(g_audio_source, "wifi", sizeof(g_audio_source));
    g_audio_source[sizeof(g_audio_source) - 1] = '\0';

    if (!g_warm_boot_after_mode_switch) {
        delay(RadioConfig::coldStartBootMs);
    }

    Serial.begin(115200);
    delay(300);
    free_uart0_from_i2s_pins();
    Serial.println();
    Serial.println(F("BendeRadio / Bender V3 bring-up"));
    Serial.printf("Reset reason: %d (1=power,3=sw,9=brownout,15/16=wdt)\n",
                  (int)esp_reset_reason());
    Serial.printf("Free heap: %u  PSRAM: %u\n", (unsigned)ESP.getFreeHeap(),
                  (unsigned)ESP.getFreePsram());
    Serial.println(F("REQUIRED: USB CDC On Boot = Enabled (BCLK=GPIO44 clashes UART0 RX)"));
    Serial.println(F("Listen: POWER 5V, USB optional only with CDC"));

    // Усилители: GPIO41 — либо mute, либо постоянно «play» (ampUseHardwareMute=false).
    amp_force_mute();

    audio_mux_init(AudioSource::Wifi);
    // I2S/громкость — в core0 после EEPROM.

    xTaskCreatePinnedToCore(core0, "Task0", 16000, NULL, 4, &Task0, 0);
    if (!g_warm_boot_after_mode_switch) {
        delay(RadioConfig::coldStartBeforeWifiMs);
    }

    Serial.printf("Board: Bender V3 | I2S BCK=%u LRC=%u DOUT=%u mute=%u | enc %u/%u/%u | mtrx CS=%u DAT=%u CLK=%u\n",
                  (unsigned)RadioConfig::i2sBclk, (unsigned)RadioConfig::i2sLrc,
                  (unsigned)RadioConfig::i2sDout, (unsigned)RadioConfig::ampMutePin,
                  (unsigned)RadioConfig::encS1, (unsigned)RadioConfig::encS2,
                  (unsigned)RadioConfig::encBtn, (unsigned)RadioConfig::mtrxCs,
                  (unsigned)RadioConfig::mtrxDat, (unsigned)RadioConfig::mtrxClk);
    Serial.printf("Batt: ADC GPIO%u | Charge: ADC GPIO%u >%umV | Mic INMP441 BCK=%u WS=%u SD=%u\n",
                  (unsigned)RadioConfig::batteryAdcPin, (unsigned)RadioConfig::chargingDetectPin,
                  (unsigned)RadioConfig::chargingDetectMinMv, (unsigned)RadioConfig::micBclkPin,
                  (unsigned)RadioConfig::micWsPin, (unsigned)RadioConfig::micDinPin);

    WifiStored w;
    nvsLoadWifi(w);
    nvsSeedDefaultsIfNeeded(w);
    String staSsid = w.staSsid.length() ? w.staSsid : String(RadioConfig::wifiSsid);
    String staPass = w.staPass.length() ? w.staPass : String(RadioConfig::wifiPass);
    String apSsid = nvsEffectiveApSsid(w);
    String apPwd = nvsEffectiveApPass(w);

    WiFi.mode(WIFI_STA);
    wifiConnecting = true;
    WiFi.begin(staSsid.c_str(), staPass.c_str());

    uint32_t tWifi = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - tWifi < 25000) {
        delay(50);
    }
    wifiConnecting = false;

    if (WiFi.status() != WL_CONNECTED) {
        // Стабильность WebUI выше в AP-only (без одновременного STA-трафика и стрима).
        WiFi.mode(WIFI_AP);
        if (apPwd.length() >= 8) {
            WiFi.softAP(apSsid.c_str(), apPwd.c_str());
        } else {
            WiFi.softAP(apSsid.c_str());
        }
    }

    Serial.println();
    if (WiFi.status() == WL_CONNECTED) {
        Serial.println(WiFi.localIP());
        Serial.println(F("SoftAP: Wi-Fi mode — 4 clicks + hold 2s (no turn) to toggle AP for setup"));
    } else {
        Serial.println(F("STA: not connected (use SoftAP for setup)"));
    }
    if (WiFi.getMode() != WIFI_STA) {
        Serial.print(F("Config AP http://"));
        Serial.println(WiFi.softAPIP());
    }

    webUiBegin();
    bender_ai_begin();
    airplay_begin();
    airplay_set_accept(play_mode_is_airplay());
    Serial.println(F("Bender AI: hold=talk; 7 clicks=sleep; 8=restart; idle 5 min=calm, 30 min=sleep"));
    Serial.println(F("Mode: 4 clicks = FM / AIR"));
    Serial.println(F("AirPlay: 1 click=pause, 2=next, 3=prev"));

    if (!(esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT0 && RadioConfig::wakeAfterSleepAnimMs > 0)) {
        change_state();
    }
    syncWifiWithAudioSilence();
}

void loop() {
    if (strcmp(g_audio_source, "bt") == 0) {
        bt_audio_tick();
        if (!radioState.state || radioState.vol <= 0 || bt_audio_needs_pairing_ui()) {
            pcm_analyzer_reset();
        }
        delay(5);
        return;
    }

    if (!airplay_owns_speaker()) {
        webUiLoop();
    } else {
        static uint32_t s_web_ms = 0;
        if ((uint32_t)(millis() - s_web_ms) >= 80u) {
            s_web_ms = millis();
            webUiLoop();
        }
    }
    bender_ai_tick();
    airplay_tick();
    // Не блокувати радіо, поки Bender лише чекає WS/LLM. I2S0 зайнятий лише коли
    // owns_speaker — інакше стрім ніколи не reconnect після фрази.
    if (bender_ai_owns_speaker() || airplay_owns_speaker()) {
        WiFi.setSleep(false);
        esp_wifi_set_ps(WIFI_PS_NONE);
        wifi_touch_activity();
        delay(1);
        return;
    }

    // Пока играет — Wi‑Fi без modem sleep (иначе то тише/то громче, то срыв буфера).
    if (radioState.state) {
        WiFi.setSleep(false);
        esp_wifi_set_ps(WIFI_PS_NONE);
        wifi_touch_activity();
        // Только снять UART0 с GPIO44. НЕ вызывать setPinout в цикле — в yoRadio его нет,
        // а повторный setPinout рвёт I2S DMA → «пшики» даже на низкой громкости.
        static uint32_t s_uart0_guard_ms = 0;
        if ((uint32_t)(millis() - s_uart0_guard_ms) > 5000u) {
            s_uart0_guard_ms = millis();
            free_uart0_from_i2s_pins();
        }
    }

    audio.loop();
    audio.loop();
    audio.loop();
    if (!radioState.state || radioState.vol <= 0 || !audio.isRunning()) {
        pcm_analyzer_reset();
    }

    // Стрим умер при включённом радио — мягкий reconnect без переинициализации I2S.
    static uint32_t s_stream_dead_ms = 0;
    if (radioState.state && WiFi.status() == WL_CONNECTED && !audio.isRunning()) {
        if (s_stream_dead_ms == 0) {
            s_stream_dead_ms = millis();
        } else if ((uint32_t)(millis() - s_stream_dead_ms) > 2500u && reconnect == nullptr) {
            s_stream_dead_ms = millis();
            reconnect = station_url_for_current();
            Serial.println(F("[Audio] stream stalled → reconnect"));
        }
    } else {
        s_stream_dead_ms = 0;
    }

    if (reconnect) {
        const char* host = reconnect;
        reconnect = nullptr;

        if (!radioState.state) {
            if (audio.isRunning()) {
                audio.stopSong();
            }
            apply_output_volume();
            syncWifiWithAudioSilence();
        } else {
            WiFi.setSleep(false);
            esp_wifi_set_ps(WIFI_PS_NONE);
            audio.setVolume(0);

            if (audio.isRunning()) {
                audio.stopSong();
            }

            // Не трогаем setPinout на каждый reconnect — иначе «захлёбы» и щелчки.
            audio.connecttohost(host);
            if (!audio.isRunning()) {
                audio.pauseResume();
            }

            pcm_analyzer_begin_stream_settle();
            {
                const uint32_t t0 = millis();
                while ((uint32_t)(millis() - t0) < RadioConfig::ampUnmuteAfterStreamMs) {
                    audio.loop();
                    delay(1);
                }
            }
            apply_output_volume();
            syncWifiWithAudioSilence();
            wifi_touch_activity();
        }
    }

    if (!radioState.state && RadioConfig::loopDelayMsWhenRadioOff > 0) {
        delay(RadioConfig::loopDelayMsWhenRadioOff);
    }
}
