#include <Arduino.h>
#include <WiFi.h>

#include "NvsConfig.h"
#include "RadioConfig.h"
#include "WebUi.h"
#include "core0.h"

TaskHandle_t Task0;

extern Data data;

volatile uint16_t g_pcm_level_adc = 0;
volatile uint8_t g_pcm_vis = 0;

// Не рисовать волну до этого времени (мс millis) — после смены потока / старта буфера.
static volatile uint32_t s_pcm_viz_unblock_ms = 0;
static uint8_t s_inst_ema = 0;
static uint32_t s_mviz_ref = RadioConfig::pcmAnalyzerRefFloor;

// PCM из декодера: m_src → m_viz → inst (0…100) → g_pcm_vis / g_pcm_level_adc.
static void pcm_vis_reset() {
    g_pcm_vis = 0;
    g_pcm_level_adc = 0;
    s_inst_ema = 0;
    s_mviz_ref = RadioConfig::pcmAnalyzerRefFloor;
}

static void pcm_vis_begin_stream_settle() {
    s_pcm_viz_unblock_ms = millis() + RadioConfig::pcmVizStreamSettleMs;
    pcm_vis_reset();
}

static void pcm_serial_debug_line(const char* tag,
                                  uint16_t len,
                                  uint32_t m,
                                  uint32_t ref,
                                  uint32_t inst_tgt,
                                  uint32_t inst_ema,
                                  uint8_t gv,
                                  uint16_t adc) {
    if (!RadioConfig::debugAudioPcmSerial) {
        return;
    }
    static uint32_t s_pcm_dbg_ms;
    const uint32_t now = millis();
    if ((int32_t)(now - s_pcm_dbg_ms) < (int32_t)RadioConfig::debugAudioPcmSerialMs) {
        return;
    }
    s_pcm_dbg_ms = now;
    Serial.printf(
        "[PCM] %s len=%u m=%lu ref=%lu tgt=%lu ema=%lu gv=%u adc=%u run=%d\n", tag, (unsigned)len,
        (unsigned long)m, (unsigned long)ref, (unsigned long)inst_tgt, (unsigned long)inst_ema, (unsigned)gv,
        (unsigned)adc, audio.isRunning() ? 1 : 0);
}

void audio_process_extern(int16_t* buff, uint16_t len, bool* continueI2S) {
    *continueI2S = true;

    if (!data.state || data.vol <= 0 || !audio.isRunning()) {
        pcm_vis_reset();
        if (RadioConfig::debugAudioPcmSerial) {
            static uint32_t s_pcm_skip_ms;
            const uint32_t now = millis();
            if ((int32_t)(now - s_pcm_skip_ms) >= 2000) {
                s_pcm_skip_ms = now;
                Serial.printf("[PCM] skip radio off: state=%d vol=%d run=%d\n", (int)data.state,
                              (int)data.vol, audio.isRunning() ? 1 : 0);
            }
        }
        return;
    }
    if ((int32_t)(millis() - s_pcm_viz_unblock_ms) < 0) {
        pcm_vis_reset();
        if (RadioConfig::debugAudioPcmSerial) {
            static uint32_t s_pcm_settle_ms;
            const uint32_t now = millis();
            if ((int32_t)(now - s_pcm_settle_ms) >= 2000) {
                s_pcm_settle_ms = now;
                Serial.println("[PCM] skip stream settle (pcmVizStreamSettleMs)");
            }
        }
        return;
    }
    if (!buff || len == 0) {
        return;
    }

    uint8_t ch = audio.getChannels();
    if (ch == 0) {
        ch = 2;
    }

    // Разреженный проход — меньше работы в колбэке декодера (меньше лагов Wi‑Fi/аудио).
    const uint16_t stride = (len > 320) ? 3u : 1u;

    uint32_t peak = 0;
    uint64_t sum_abs = 0;
    uint32_t cnt = 0;
    if (ch >= 2) {
        for (uint16_t i = 0; i < len; i += stride) {
            const int32_t m = ((int32_t)buff[i * 2] + (int32_t)buff[i * 2 + 1]) >> 1;
            const uint32_t a = (uint32_t)(m >= 0 ? m : -m);
            sum_abs += a;
            ++cnt;
            if (a > peak) {
                peak = a;
            }
        }
    } else {
        for (uint16_t i = 0; i < len; i += stride) {
            const int32_t m = buff[i];
            const uint32_t a = (uint32_t)(m >= 0 ? m : -m);
            sum_abs += a;
            ++cnt;
            if (a > peak) {
                peak = a;
            }
        }
    }
    if (cnt == 0) {
        return;
    }

    const uint32_t mean_abs = (uint32_t)(sum_abs / cnt);
    const uint32_t body = (mean_abs * 3u) >> 1;
    const uint32_t m_src = (peak > body) ? peak : body;

    // Волна / рот / шкала — от сырой m_src; порог тишины и полная шкала — RadioConfig (под int16 PCM).
    const uint32_t m_viz = (m_src >= RadioConfig::pcmSilenceAbs) ? m_src : 0u;

    if (RadioConfig::pcmUseAdaptiveAnalyzerRef) {
        if (m_viz > 0u) {
            if (m_viz > s_mviz_ref) {
                const uint32_t gap = m_viz - s_mviz_ref;
                const uint32_t step = max(1u, gap >> RadioConfig::pcmAnalyzerRefAttackShift);
                s_mviz_ref = min((uint32_t)RadioConfig::pcmAnalyzerRefCeil, s_mviz_ref + step);
            } else {
                const uint32_t sub = (s_mviz_ref * (uint32_t)RadioConfig::pcmAnalyzerRefRelease) >> 8;
                s_mviz_ref = (sub >= s_mviz_ref)
                                 ? RadioConfig::pcmAnalyzerRefFloor
                                 : max(RadioConfig::pcmAnalyzerRefFloor, s_mviz_ref - sub);
            }
        } else {
            const uint32_t sub = (s_mviz_ref * 6u) >> 8;
            s_mviz_ref = (sub >= s_mviz_ref) ? RadioConfig::pcmAnalyzerRefFloor
                                             : max(RadioConfig::pcmAnalyzerRefFloor, s_mviz_ref - sub);
        }
    }

    const uint32_t ref_div = RadioConfig::pcmUseAdaptiveAnalyzerRef
                                 ? max(RadioConfig::pcmAnalyzerRefFloor, s_mviz_ref)
                                 : RadioConfig::pcmMetricFullScale;
    const uint32_t ref_dbg =
        RadioConfig::pcmUseAdaptiveAnalyzerRef ? s_mviz_ref : RadioConfig::pcmMetricFullScale;
    const uint8_t inst_target =
        m_viz > 0u ? (uint8_t)min(100u, m_viz * 100u / ref_div) : (uint8_t)0;

    {
        const uint8_t sh = RadioConfig::pcmInstSmoothShift;
        const uint32_t num = (1u << sh) - 1u;
        const uint32_t acc = (uint32_t)s_inst_ema * num + (uint32_t)inst_target;
        s_inst_ema = (uint8_t)(acc >> sh);
    }
    const uint8_t inst = s_inst_ema;

    const uint8_t gv = g_pcm_vis;
    if (inst == 0) {
        g_pcm_vis = (uint8_t)((gv * 5u) >> 3);
        g_pcm_level_adc = (uint16_t)((g_pcm_level_adc * 5u) >> 3);
        pcm_serial_debug_line("silent", len, m_src, ref_dbg, 0u, 0u, gv, g_pcm_level_adc);
        return;
    }

    if (inst >= gv) {
        g_pcm_vis = (uint8_t)((gv * 2u + inst * 6u) >> 3);
    } else {
        g_pcm_vis = (uint8_t)((gv * 11u + inst * 5u) >> 4);
    }

    uint32_t adc = (uint32_t)inst * 4095u / 100u;
    if (adc > 4095u) {
        adc = 4095u;
    }
    g_pcm_level_adc = (uint16_t)adc;

    pcm_serial_debug_line("ok", len, m_viz, ref_dbg, inst_target, inst, g_pcm_vis, g_pcm_level_adc);
}

void setup() {
    delay(RadioConfig::coldStartBootMs);
    xTaskCreatePinnedToCore(core0, "Task0", 10000, NULL, 1, &Task0, 0);

    Serial.begin(115200);
    delay(RadioConfig::coldStartBeforeWifiMs);

    WifiStored w;
    nvsLoadWifi(w);
    nvsSeedDefaultsIfNeeded(w);
    String staSsid = w.staSsid.length() ? w.staSsid : String(RadioConfig::wifiSsid);
    String staPass = w.staPass.length() ? w.staPass : String(RadioConfig::wifiPass);
    String apSsid = nvsEffectiveApSsid(w);
    String apPwd = nvsEffectiveApPass(w);

    WiFi.mode(WIFI_AP_STA);
    if (apPwd.length() >= 8) {
        WiFi.softAP(apSsid.c_str(), apPwd.c_str());
    } else {
        WiFi.softAP(apSsid.c_str());
    }

    wifiConnecting = true;
    WiFi.begin(staSsid.c_str(), staPass.c_str());

    uint32_t tWifi = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - tWifi < 25000) {
        delay(50);
    }
    wifiConnecting = false;

    Serial.println();
    if (WiFi.status() == WL_CONNECTED) {
        Serial.println(WiFi.localIP());
    } else {
        Serial.println(F("STA: not connected (use SoftAP for setup)"));
    }
    Serial.print(F("Config AP http://"));
    Serial.println(WiFi.softAPIP());

    webUiBegin();

    change_state();
    syncWifiWithAudioSilence();
}

void loop() {
    webUiLoop();
    audio.loop();
    if (!data.state || data.vol <= 0 || !audio.isRunning()) {
        pcm_vis_reset();
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

        pcm_vis_begin_stream_settle();

        audio.setVolume(data.state ? data.vol : 0);
        if (!data.state) {
            audio.stopSong();
        }
        syncWifiWithAudioSilence();
    }
}
