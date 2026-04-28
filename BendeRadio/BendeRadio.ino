#include <Arduino.h>
#include <cstring>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_sleep.h>
#include <esp_system.h>
#include "BtAudio.h"
#include "NvsConfig.h"
#include "RadioConfig.h"
#include "WebUi.h"
#include "core0.h"

TaskHandle_t Task0;

char g_audio_source[8] = "wifi";
bool g_warm_boot_after_mode_switch = false;

void commitSourceModeSwitch(const char* new_mode) {
    if (strcmp(new_mode, "wifi") != 0 && strcmp(new_mode, "bt") != 0) {
        return;
    }
    Preferences prefs;
    prefs.begin("bende", false);
    prefs.putString("aud", new_mode);
    prefs.putBool("wmrst", true);
    prefs.end();
    delay(RadioConfig::modeSwitchRestartDelayMs);
    esp_restart();
}

extern Data data;

void audio_process_extern(int16_t* buff, uint16_t len, bool* continueI2S) {
    *continueI2S = true;

    if (strcmp(g_audio_source, "bt") == 0) {
        return;
    }

    uint8_t ch = audio.getChannels();
    if (ch == 0) {
        ch = 2;
    }
    pcm_analyzer_on_decoder_buffer(buff, len, ch, audio.isRunning());
}

void setup() {
    // Сначала NVS (до core0 и до холодных delay): режим + флаг «тёплой» перезагрузки после смены Wi‑Fi/BT.
    {
        Preferences prefs;
        prefs.begin("bende", true);
        g_warm_boot_after_mode_switch = prefs.getBool("wmrst", false);
        String s = prefs.getString("aud", "");
        if (s != "wifi" && s != "bt") {
            const uint8_t legacy = prefs.getUChar("src", 0);
            s = (legacy == 1) ? "bt" : "wifi";
            prefs.end();
            prefs.begin("bende", false);
            prefs.putString("aud", s);
            prefs.end();
        } else {
            prefs.end();
        }
        strncpy(g_audio_source, s.c_str(), sizeof(g_audio_source));
        g_audio_source[sizeof(g_audio_source) - 1] = '\0';
        if (g_warm_boot_after_mode_switch) {
            prefs.begin("bende", false);
            prefs.putBool("wmrst", false);
            prefs.end();
        }
    }
    if (strcmp(g_audio_source, "wifi") != 0 && strcmp(g_audio_source, "bt") != 0) {
        strncpy(g_audio_source, "wifi", sizeof(g_audio_source));
        g_audio_source[sizeof(g_audio_source) - 1] = '\0';
    }

    if (!g_warm_boot_after_mode_switch) {
        delay(RadioConfig::coldStartBootMs);
    }

    xTaskCreatePinnedToCore(core0, "Task0", 10000, NULL, 1, &Task0, 0);

    Serial.begin(115200);
    if (!g_warm_boot_after_mode_switch) {
        delay(RadioConfig::coldStartBeforeWifiMs);
    }

    if (strcmp(g_audio_source, "bt") == 0) {
        wifiConnecting = false;
        Serial.println(F("Mode: Bluetooth A2DP (pair from phone)"));
        bt_audio_start_sink();
        bt_audio_volume_apply(data.state, data.vol);
        if (!(esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT0 && RadioConfig::wakeAfterSleepAnimMs > 0)) {
            change_state();
        }
        syncWifiWithAudioSilence();
        return;
    }

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

    if (!(esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT0 && RadioConfig::wakeAfterSleepAnimMs > 0)) {
        change_state();
    }
    syncWifiWithAudioSilence();
}

void loop() {
    if (strcmp(g_audio_source, "bt") == 0) {
        bt_audio_tick();
        if (!data.state || data.vol <= 0 || bt_audio_needs_pairing_ui()) {
            pcm_analyzer_reset();
        }
        delay(5);
        return;
    }

    webUiLoop();
    audio.loop();
    if (!data.state || data.vol <= 0 || !audio.isRunning()) {
        pcm_analyzer_reset();
    }

    if (reconnect) {
        const char* host = reconnect;
        reconnect = nullptr;

        if (audio.isRunning()) {
            audio.setVolume(0);
            uint32_t t0 = millis();
            while (audio.isRunning() && millis() - t0 < RadioConfig::audioPauseSilenceRampMs) {
                audio.loop();
                delay(2);
            }
            if (audio.isRunning()) {
                audio.pauseResume();
            }
        }

        audio.connecttohost(host);
        if (!audio.isRunning()) {
            audio.pauseResume();
        }

        pcm_analyzer_begin_stream_settle();

        audio.setVolume(data.state ? data.vol : 0);
        if (!data.state) {
            audio.stopSong();
        }
        syncWifiWithAudioSilence();
    }

    if (!data.state && RadioConfig::loopDelayMsWhenRadioOff > 0) {
        delay(RadioConfig::loopDelayMsWhenRadioOff);
    }
}
