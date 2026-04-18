#include "pcm_analyzer.h"

#include "RadioConfig.h"
#include "core0.h"

#include <cstdint>
#include <math.h>

extern Data data;

volatile uint16_t g_pcm_level_adc = 0;
volatile uint8_t g_pcm_vis = 0;
volatile uint8_t g_pcm_eq_band[RadioConfig::pcmEqBandCount];

static volatile uint32_t s_pcm_viz_unblock_ms = 0;
static uint8_t s_inst_ema = 0;
static uint32_t s_mviz_ref = RadioConfig::pcmAnalyzerRefFloor;
static uint8_t s_eq_ema[RadioConfig::pcmEqBandCount];
static float s_eq_decorrel_phase = 0.f;

void pcm_analyzer_reset() {
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

void pcm_analyzer_begin_stream_settle() {
    s_pcm_viz_unblock_ms = millis() + RadioConfig::pcmVizStreamSettleMs;
    pcm_analyzer_reset();
}

static uint8_t pcm_vis_stretch(uint8_t x) {
    if (!RadioConfig::pcmVisSqrtStretch || x == 0) {
        return x;
    }
    const uint32_t y = (uint32_t)(10.f * sqrtf((float)x));
    return (uint8_t)min(100u, y);
}

static int32_t pcm_gain_sample(int32_t s, uint16_t gain_pct) {
    if (gain_pct == 100u) {
        return s;
    }
    const int64_t m = (int64_t)s * (int64_t)gain_pct / 100LL;
    if (m > 32767) {
        return 32767;
    }
    if (m < -32768) {
        return -32768;
    }
    return (int32_t)m;
}

static void pcm_serial_debug_line(const char* tag,
                                  uint16_t len,
                                  uint32_t m,
                                  uint32_t ref,
                                  uint32_t inst_tgt,
                                  uint32_t inst_ema,
                                  uint8_t gv,
                                  uint16_t adc,
                                  int run_flag) {
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
        (unsigned)adc, run_flag);
}

static void pcm_analyzer_feed_impl(const int16_t* buff,
                                   uint16_t len,
                                   uint8_t ch,
                                   int dbg_run_flag,
                                   uint16_t linear_gain_pct,
                                   uint32_t silence_abs_threshold) {
    if (!buff || len == 0) {
        return;
    }
    if (ch == 0) {
        ch = 2;
    }
    if (linear_gain_pct == 0u) {
        linear_gain_pct = 100u;
    }

    const uint16_t stride = (len > 320) ? 3u : 1u;

    uint32_t peak = 0;
    uint64_t sum_abs = 0;
    uint32_t cnt = 0;
    if (ch >= 2) {
        for (uint16_t i = 0; i < len; i += stride) {
            const int32_t raw = ((int32_t)buff[i * 2] + (int32_t)buff[i * 2 + 1]) >> 1;
            const int32_t m = pcm_gain_sample(raw, linear_gain_pct);
            const uint32_t a = (uint32_t)(m >= 0 ? m : -m);
            sum_abs += a;
            ++cnt;
            if (a > peak) {
                peak = a;
            }
        }
    } else {
        for (uint16_t i = 0; i < len; i += stride) {
            const int32_t m = pcm_gain_sample((int32_t)buff[i], linear_gain_pct);
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

    const uint32_t m_viz = (m_src >= silence_abs_threshold) ? m_src : 0u;

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
        pcm_serial_debug_line("silent", len, m_src, ref_dbg, 0u, 0u, gv, g_pcm_level_adc, dbg_run_flag);
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
                    const int32_t raw = ((int32_t)buff[ii * 2] + (int32_t)buff[ii * 2 + 1]) >> 1;
                    mono = pcm_gain_sample(raw, linear_gain_pct);
                } else {
                    mono = pcm_gain_sample((int32_t)buff[ii], linear_gain_pct);
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

    pcm_serial_debug_line("ok", len, m_viz, ref_dbg, inst_target, inst, g_pcm_vis, g_pcm_level_adc,
                          dbg_run_flag);
}

void pcm_analyzer_on_decoder_buffer(int16_t* buff, uint16_t len_frames, uint8_t ch, bool stream_running) {
    if (!data.state || data.vol <= 0 || !stream_running) {
        pcm_analyzer_reset();
        if (RadioConfig::debugAudioPcmSerial) {
            static uint32_t s_pcm_skip_ms;
            const uint32_t now = millis();
            if ((int32_t)(now - s_pcm_skip_ms) >= 2000) {
                s_pcm_skip_ms = now;
                Serial.printf("[PCM] skip radio off: state=%d vol=%d run=%d\n", (int)data.state,
                              (int)data.vol, stream_running ? 1 : 0);
            }
        }
        return;
    }
    if ((int32_t)(millis() - s_pcm_viz_unblock_ms) < 0) {
        pcm_analyzer_reset();
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
    pcm_analyzer_feed_impl(buff, len_frames, ch, stream_running ? 1 : 0, 100u, RadioConfig::pcmSilenceAbs);
}

void pcm_analyzer_on_bt_pcm_bytes(const uint8_t* pcm_bytes, uint32_t len_bytes) {
    if (!pcm_bytes || len_bytes < 4u) {
        return;
    }
    if (!data.state || data.vol <= 0) {
        pcm_analyzer_reset();
        return;
    }
    if ((int32_t)(millis() - s_pcm_viz_unblock_ms) < 0) {
        pcm_analyzer_reset();
        return;
    }
    const uint32_t n_frames = len_bytes / 4u;
    if (n_frames == 0) {
        return;
    }
    const int16_t* s = reinterpret_cast<const int16_t*>(pcm_bytes);
    pcm_analyzer_feed_impl(s, (uint16_t)min((uint32_t)UINT16_MAX, n_frames), 2, 1,
                           RadioConfig::btPcmAnalyzerGainPercent, RadioConfig::btPcmSilenceAbs);
}
