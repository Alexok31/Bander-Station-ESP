#include <Arduino.h>
#include <WiFi.h>
#include <esp_sleep.h>
#include <math.h>

#include "NvsConfig.h"
#include "RadioConfig.h"
#include "WebUi.h"
#include "core0.h"

TaskHandle_t Task0;

extern Data data;

volatile uint16_t g_pcm_level_adc = 0;
volatile uint8_t g_pcm_vis = 0;
volatile uint8_t g_pcm_eq_band[RadioConfig::pcmEqBandCount];

// Не рисовать волну до этого времени (мс millis) — после смены потока / старта буфера.
static volatile uint32_t s_pcm_viz_unblock_ms = 0;
static uint8_t s_inst_ema = 0;
static uint32_t s_mviz_ref = RadioConfig::pcmAnalyzerRefFloor;
static uint8_t s_eq_ema[RadioConfig::pcmEqBandCount];
static float s_eq_decorrel_phase = 0.f;

// PCM из декодера: m_src → m_viz → inst (0…100) → g_pcm_vis / g_pcm_level_adc; сегменты буфера → g_pcm_eq_band.
static void pcm_vis_reset() {
    g_pcm_vis = 0;
    g_pcm_level_adc = 0;
    s_inst_ema = 0;
    s_mviz_ref = RadioConfig::pcmAnalyzerRefFloor;
    s_eq_decorrel_phase = 0.f;
    for (uint8_t b = 0; b < (uint8_t)RadioConfig::pcmEqBandCount; b++) {
        s_eq_ema[b] = 0;
        g_pcm_eq_band[b] = 0;
    }
}

static void pcm_vis_begin_stream_settle() {
    s_pcm_viz_unblock_ms = millis() + RadioConfig::pcmVizStreamSettleMs;
    pcm_vis_reset();
}

// Усиливает малые значения уровня (после AGC inst часто 5…25 из 100).
static uint8_t pcm_vis_stretch(uint8_t x) {
    if (!RadioConfig::pcmVisSqrtStretch || x == 0) {
        return x;
    }
    const uint32_t y = (uint32_t)(10.f * sqrtf((float)x));
    return (uint8_t)min(100u, y);
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
        for (uint8_t b = 0; b < (uint8_t)RadioConfig::pcmEqBandCount; b++) {
            s_eq_ema[b] = (uint8_t)(((uint32_t)s_eq_ema[b] * 5u) >> 3);
            g_pcm_eq_band[b] = s_eq_ema[b];
        }
        pcm_serial_debug_line("silent", len, m_src, ref_dbg, 0u, 0u, gv, g_pcm_level_adc);
        return;
    }

    uint8_t vis_blend;
    if (inst >= gv) {
        vis_blend = (uint8_t)((gv * 2u + inst * 6u) >> 3);
    } else {
        vis_blend = (uint8_t)((gv * 11u + inst * 5u) >> 4);
    }
    g_pcm_vis = pcm_vis_stretch(vis_blend);

    const uint32_t adc_full = (uint32_t)RadioConfig::pcmLevelAdcMax;
    uint32_t adc = (uint32_t)inst * adc_full / 100u;
    if (adc > adc_full) {
        adc = adc_full;
    }
    g_pcm_level_adc = (uint16_t)adc;

    {
        const int B = RadioConfig::pcmEqBandCount;
        if (RadioConfig::pcmEqDecorrelAmount > 0.f) {
            s_eq_decorrel_phase +=
                RadioConfig::pcmEqDecorrelOmega * (1.f + (float)len / 1536.f);
            while (s_eq_decorrel_phase > 6.2831855f) {
                s_eq_decorrel_phase -= 6.2831855f;
            }
        }
        for (int b = 0; b < B; b++) {
            const uint8_t sh = (uint8_t)min(
                7, (int)RadioConfig::pcmEqBandSmoothShift + (int)((unsigned)b * 7u & 3u) +
                         (int)(RadioConfig::pcmEqBandStaggerSmooth & (uint8_t)(b & 1)));
            const uint32_t num = (1u << sh) - 1u;
            uint32_t pkb = 0;
            for (uint32_t ii = (uint32_t)b; ii < (uint32_t)len; ii += (uint32_t)B) {
                int32_t mono;
                if (ch >= 2) {
                    mono = ((int32_t)buff[ii * 2] + (int32_t)buff[ii * 2 + 1]) >> 1;
                } else {
                    mono = buff[ii];
                }
                const uint32_t a = (uint32_t)(mono >= 0 ? mono : -mono);
                if (a > pkb) {
                    pkb = a;
                }
            }
            uint8_t tgt = 0;
            if (m_viz > 0u && ref_div > 0u) {
                tgt = (uint8_t)min(100u, pkb * 100u / ref_div);
            }
            if (RadioConfig::pcmEqDecorrelAmount > 0.f && tgt > 0u) {
                const float a = RadioConfig::pcmEqDecorrelAmount;
                const float bf = (float)b;
                const float u =
                    0.5f +
                    0.25f * (sinf(s_eq_decorrel_phase + bf * RadioConfig::pcmEqDecorrelColSpread) +
                             sinf(s_eq_decorrel_phase * 1.618f + bf * RadioConfig::pcmEqDecorrelColSpread2));
                float clamped = u;
                if (clamped < 0.f) {
                    clamped = 0.f;
                } else if (clamped > 1.f) {
                    clamped = 1.f;
                }
                const float mul = (1.f - a) + a * clamped;
                tgt = (uint8_t)min(100u, (uint32_t)((float)tgt * mul + 0.5f));
            }
            const uint32_t acc = (uint32_t)s_eq_ema[b] * num + (uint32_t)tgt;
            s_eq_ema[b] = (uint8_t)(acc >> sh);
            g_pcm_eq_band[b] = s_eq_ema[b];
        }
    }

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

    WiFi.mode(WIFI_STA);
    wifiConnecting = true;
    WiFi.begin(staSsid.c_str(), staPass.c_str());

    uint32_t tWifi = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - tWifi < 25000) {
        delay(50);
    }
    wifiConnecting = false;

    if (WiFi.status() != WL_CONNECTED) {
        WiFi.mode(WIFI_AP_STA);
        if (apPwd.length() >= 8) {
            WiFi.softAP(apSsid.c_str(), apPwd.c_str());
        } else {
            WiFi.softAP(apSsid.c_str());
        }
    }

    Serial.println();
    if (WiFi.status() == WL_CONNECTED) {
        Serial.println(WiFi.localIP());
        Serial.println(F("SoftAP: off (6 clicks to enable AP for setup)"));
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

    if (!data.state && RadioConfig::loopDelayMsWhenRadioOff > 0) {
        delay(RadioConfig::loopDelayMsWhenRadioOff);
    }
}
