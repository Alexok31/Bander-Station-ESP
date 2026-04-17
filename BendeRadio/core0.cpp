#include "core0.h"

#include <math.h>
#include <ESP.h>
#include <WiFi.h>
#include <esp_sleep.h>
#include <esp_wifi.h>

#include <EEManager.h>
#include <EncButton.h>
#include <FastLED.h>
#include <GyverMAX7219.h>

#include "battery.h"
#include "NvsConfig.h"
#include "pong.h"
#include "soc/timer_group_reg.h"
#include "soc/timer_group_struct.h"
#include "tmr.h"

static inline uint8_t mouth_gfx_on(bool invert) {
    return invert ? GFX_CLEAR : GFX_FILL;
}
static inline uint8_t mouth_gfx_off(bool invert) {
    return invert ? GFX_FILL : GFX_CLEAR;
}

const char* stations[] = {
    "https://uk3.internet-radio.com/proxy/majesticjukebox?mp=/live",
    "http://prmstrm.1.fm:8000/electronica",
    "http://prmstrm.1.fm:8000/x",
    "http://stream81.metacast.eu/radio1rock128",
};

// data
MAX7219<5, 1, RadioConfig::mtrxCs, RadioConfig::mtrxDat, RadioConfig::mtrxClk> mtrx;
Tmr square_tmr;
Data data;
EEManager memory(data);
Audio audio;
String streamname;
const char* reconnect = nullptr;
volatile bool wifiConnecting = false;

static uint32_t s_wake_after_sleep_anim_until_ms = 0;
static bool s_pending_change_state_after_wake = false;

// func
// ========================= MATRIX =========================
void upd_bright() {
    uint8_t m = data.bright_mouth, e = data.bright_eyes;
    uint8_t br[] = {m, m, m, e, e};
    mtrx.setBright(br);
}

// Pong: на табло ярче крайние «ротовые» модули; счёт на глазах — подсвечиваем их (bright_eyes).
static void pong_sync_matrix_brightness() {
    if (pong_mouth_tablo_mode()) {
        uint8_t m = (uint8_t)constrain((int)data.bright_mouth, 0, 15);
        uint8_t side = (uint8_t)constrain((int)m + 5, 0, 15);
        uint8_t mid = (uint8_t)constrain((int)m - 2, 0, 15);
        const uint8_t pulse = (uint8_t)(((millis() / 90) & 1) ? 1u : 0u);
        side = (uint8_t)constrain((int)side + (int)pulse, 0, 15);
        uint8_t e = (uint8_t)constrain((int)data.bright_eyes, 0, 15);
        uint8_t br[] = {side, mid, side, e, e};
        mtrx.setBright(br);
    } else {
        upd_bright();
    }
}
void print_val(char c, uint8_t v) {
    // Завжди тёмный фон + светлый шрифт: библиотечный print не умеет «тёмные» глифы при інверсії рота.
    mtrx.rect(0, 0, RadioConfig::analyzWidth - 1, 7, GFX_CLEAR);
    mtrx.setCursor(8 * 0 + 2, 1);
    mtrx.print(c);
    mtrx.setCursor(8 * 1 + 2, 1);
    mtrx.print(v / 10);
    mtrx.setCursor(8 * 2 + 2, 1);
    mtrx.print(v % 10);
    mtrx.update();
}

static void draw_batt_lightning_glyph() {
    // Тонкий зигзаг (1 px), біт 7 = лівий стовпчик x=0. Увесь гліф опущено на 1 рядок вниз.
    static const uint8_t kRows[8] = {
        0x00,
        0x08,  // верхня діагональ (зсув вліво на 1)
        0x10,
        0x20,
        0x3C,  // x=2…5 горизонталь
        0x04,  // x=5
        0x08,  // x=4
        0x10,  // x=3
    };
    for (int y = 0; y < 8; y++) {
        uint8_t b = kRows[y];
        for (int x = 0; x < 8; x++) {
            if (b & (uint8_t)(1 << (7 - x))) {
                mtrx.dot((uint8_t)x, (uint8_t)y, GFX_FILL);
            }
        }
    }
}

void print_batt(uint8_t pct) {
    const uint8_t v = (pct > 99u) ? 99u : pct;
    mtrx.rect(0, 0, RadioConfig::analyzWidth - 1, 7, GFX_CLEAR);
    draw_batt_lightning_glyph();
    mtrx.setCursor(8 * 1 + 2, 1);
    mtrx.print((char)('0' + (v / 10)));
    mtrx.setCursor(8 * 2 + 2, 1);
    mtrx.print((char)('0' + (v % 10)));
    mtrx.update();
}

// ========================= EYES =========================
void draw_eye(uint8_t i) {
    uint8_t x = RadioConfig::analyzWidth + i * 8;
    mtrx.rect(1 + x, 1, 6 + x, 6, GFX_FILL);
    mtrx.lineV(0 + x, 2, 5);
    mtrx.lineV(7 + x, 2, 5);
    mtrx.lineH(0, 2 + x, 5 + x);
    mtrx.lineH(7, 2 + x, 5 + x);
}
void draw_eyeb(uint8_t i, int x, int y, int w = 2) {
    x += RadioConfig::analyzWidth + i * 8;
    mtrx.rect(x, y, x + w - 1, y + w - 1, GFX_CLEAR);
}

// Pong: на табло счёт на глазах (левый — игрок, правый — ИИ). В розыгрыше — зрачок следует за мячом.
static void draw_eyes_follow_ball(int8_t ball_x, int8_t ball_y) {
    if (pong_mouth_tablo_mode()) {
        const uint8_t bx = RadioConfig::analyzWidth;
        mtrx.rect(bx, 0, bx + 16 - 1, 7, GFX_CLEAR);
        mtrx.setCursor(bx + 2, 1);
        mtrx.print((char)('0' + (pong_score_player() % 10)));
        mtrx.setCursor(bx + 8 + 2, 1);
        mtrx.print((char)('0' + (pong_score_ai() % 10)));
        return;
    }

    if (!data.state) {
        draw_eye(0);
        draw_eye(1);
        mtrx.rect(RadioConfig::analyzWidth, 0, RadioConfig::analyzWidth + 16 - 1, 3, GFX_CLEAR);
        draw_eyeb(0, 3, 5);
        draw_eyeb(1, 3, 5);
        return;
    }
    const int8_t pw = RadioConfig::analyzWidth;

    for (uint8_t i = 0; i < 2; i++) {
        draw_eye(i);
        const uint8_t base = RadioConfig::analyzWidth + i * 8;
        const uint8_t pup_x = (uint8_t)map(ball_x, 0, pw - 1, 2, 4);
        const uint8_t pup_y = (uint8_t)map(ball_y, 0, 7, 2, 4);
        int16_t dx = 0;
        if (ball_x < pw / 2) {
            dx = (i == 0) ? -1 : 0;
        } else if (ball_x > pw / 2) {
            dx = (i == 0) ? 0 : 1;
        }
        const uint8_t px = (uint8_t)constrain((int)pup_x + dx, 2, 4);
        mtrx.rect(base + px, pup_y, base + px + 1, pup_y + 1, GFX_CLEAR);
    }
}

void anim_search() {
    static int8_t pos = 4, dir = 1;
    static Tmr tmr(50);
    if (tmr) {
        pos += dir;
        if (pos >= 6) dir = -1;
        if (pos <= 0) dir = 1;
        mtrx.rect(RadioConfig::analyzWidth, 2, RadioConfig::analyzWidth + 16 - 1, 5, GFX_FILL);
        draw_eyeb(0, pos, 3);
        draw_eyeb(1, pos, 3);
        mtrx.update();
    }
}
void change_state() {
    mtrx.clear();
    if (data.state) {
        upd_bright();
        square_tmr.start(600);
        draw_eye(0);
        draw_eye(1);
        draw_eyeb(0, 2, 2, 4);
        draw_eyeb(1, 2, 2, 4);
    } else {
        mtrx.setBright((uint8_t)0);
        draw_eye(0);
        draw_eye(1);
        mtrx.rect(RadioConfig::analyzWidth, 0, RadioConfig::analyzWidth + 16 - 1, 3, GFX_CLEAR);
        draw_eyeb(0, 3, 5);
        draw_eyeb(1, 3, 5);
    }
    mtrx.update();
}

// ========================= ANALYZ =========================
// g_pcm_level_adc в 0…4095 (inst*4095/100 в BendeRadio.ino). Порог тишины data.trsh — в тех же единицах.
// Раньше нарастание было +120 счётчиков ADC — при полной шкале 4095 это ~3% над порогом → «рот» почти всегда 1 px.
static uint8_t pcm_vis_after_noise_gate(uint8_t vw) {
    const uint16_t adc = g_pcm_level_adc;
    if (adc <= data.trsh || vw == 0) {
        return 0;
    }
    if (RadioConfig::pcmNoiseGateBinary) {
        return vw;
    }
    const uint32_t above = (uint32_t)adc - (uint32_t)data.trsh;
    const uint32_t head = (uint32_t)RadioConfig::pcmLevelAdcMax - (uint32_t)data.trsh;
    uint32_t ramp = head / 3u;
    if (ramp < 200u) {
        ramp = 200u;
    }
    if (ramp > 1200u) {
        ramp = 1200u;
    }
    if (above < ramp) {
        return (uint8_t)((uint32_t)vw * above / ramp);
    }
    return vw;
}

static uint8_t pcm_wave_level_after_gate() {
    return pcm_vis_after_noise_gate(g_pcm_vis);
}

// Режим 1: колонки 1 px; фиксированные веса по X (две «горки» sin, произведение) — без бегущей фазы, движение только от PCM.
static void analyz_eq_bars(uint8_t v_gate, bool invert) {
    const float floor = RadioConfig::pcmEqShapeFloor;
    const float span = 1.f - floor;
    const float deep = RadioConfig::pcmEqShapeDeep;
    const float k1 = RadioConfig::pcmEqShapeK1;
    const float k2 = RadioConfig::pcmEqShapeK2;
    const float p1 = RadioConfig::pcmEqShapeP1;
    const float p2 = RadioConfig::pcmEqShapeP2;

    const int W = RadioConfig::analyzWidth;
    const int B = RadioConfig::pcmEqBandCount;
    const int n = (W < B) ? W : B;
    for (int col = 0; col < n; col++) {
        const uint8_t raw = g_pcm_eq_band[col];
        const uint32_t gated = (uint32_t)raw * (uint32_t)v_gate / 100u;
        const float c = (float)col;
        const float w1 = 0.5f + 0.5f * sinf(k1 * c + p1);
        const float w2 = 0.5f + 0.5f * sinf(k2 * c + p2);
        const float t = fmaxf(w1 * w2, RadioConfig::pcmEqShapeTMin);
        const float env = floor + span * (deep + (1.f - deep) * t);
        uint32_t shaped = (uint32_t)((float)gated * env + 0.5f);
        if (shaped > 100u) {
            shaped = 100u;
        }
        int h = (int)((shaped * 8u + 99u) / 100u);
        if (h > 8) {
            h = 8;
        }
        if (h <= 0) {
            continue;
        }
        const int yTop = 8 - h;
        mtrx.rect(col, yTop, col, 7, mouth_gfx_on(invert));
    }
}

void analyz0(uint8_t vol, bool invert) {
    static float phi;
    static float phi_chaos;
    static float omega_filt;
    constexpr float two_pi = 6.2831853f;

    const float omega_tgt = RadioConfig::analyzSineOmegaMin +
                            (float)vol / 100.f * (RadioConfig::analyzSineOmegaMax - RadioConfig::analyzSineOmegaMin);
    const float ease = RadioConfig::analyzSineOmegaEase;
    omega_filt += (omega_tgt - omega_filt) * ease;
    phi += omega_filt;
    phi_chaos += omega_filt * RadioConfig::analyzWaveChaosOmegaRatio;
    while (phi > two_pi * 16.f) {
        phi -= two_pi * 16.f;
    }
    while (phi_chaos > two_pi * 24.f) {
        phi_chaos -= two_pi * 24.f;
    }

    const int W = RadioConfig::analyzWidth;
    const float k = two_pi * RadioConfig::analyzSinePeriodsAcross / (float)W;
    const float k2 = RadioConfig::analyzWaveChaosK2;
    const float fm = RadioConfig::analyzWaveFmDepth;
    const float nmix = RadioConfig::analyzWaveNoiseMix;
    const float mid = 3.5f + (float)RadioConfig::analyzWaveRowOffset;
    const float amp = (float)vol / 100.f * RadioConfig::analyzSineAmpMax;

    int8_t rows[32];
    for (int i = 0; i < W; i++) {
        const float inner = sinf(phi_chaos + k2 * (float)i);
        float y = mid + amp * sinf(phi + k * (float)i + fm * inner);
        if (nmix > 0.f && amp > 0.05f) {
            const uint8_t nz = inoise8((uint8_t)(i * 19 + 7), (uint8_t)(phi * 40.f + phi_chaos * 13.f));
            y += ((float)nz / 255.f - 0.5f) * 2.f * amp * nmix;
        }
        int r = (int)roundf(y);
        rows[i] = (int8_t)constrain(r, 0, 7);
    }

    if (W <= 1) {
        mtrx.dot(0, (uint8_t)rows[0], mouth_gfx_on(invert));
        return;
    }
    for (int i = 0; i < W - 1; i++) {
        mtrx.line(i, (int)rows[i], i + 1, (int)rows[i + 1], mouth_gfx_on(invert));
    }
}

namespace {

struct MouthRobotCtx {
    float v;
    float phi;
    float phi2;
    float phi_slow;
    float nz;
    float chomp;
    float lip_wobble;
    float extra_open;
    float ripple;
    float bob;
    float kk;
    float mid;
    float sep_base;
    int W;
    int L;
    int u_fix;
    int l_fix;
    int min_gap;
    int iw;
    uint8_t curve_kind;
};

static void mouth_robot_fill_ctx(uint8_t vol, MouthRobotCtx* c) {
    static float phi;
    static float phi2;
    static float phi_slow;
    constexpr float two_pi = 6.2831853f;

    c->v = (float)vol / 100.f;
    const float v = c->v;
    const float omega = RadioConfig::analyzMouthPhiOmegaMin +
                        v * (RadioConfig::analyzMouthPhiOmegaMax - RadioConfig::analyzMouthPhiOmegaMin);
    phi += omega;
    while (phi > two_pi * 8.f) {
        phi -= two_pi * 8.f;
    }
    const float o2 = RadioConfig::analyzMouthPhi2OmegaMin +
                     v * (RadioConfig::analyzMouthPhi2OmegaMax - RadioConfig::analyzMouthPhi2OmegaMin);
    const float nz = (float)inoise8((uint8_t)(phi2 * 37.f + phi * 11.f), (uint8_t)(millis() >> 5)) / 255.f;
    const float na = fminf(0.95f, fmaxf(0.f, RadioConfig::analyzMouthOmegaNoiseAmp));
    phi2 += o2 * (1.f - na + na * (0.38f + 0.62f * nz));
    while (phi2 > two_pi * 8.f) {
        phi2 -= two_pi * 8.f;
    }
    const float o_s = RadioConfig::analyzMouthSlowOmegaMin +
                      v * (RadioConfig::analyzMouthSlowOmegaMax - RadioConfig::analyzMouthSlowOmegaMin);
    phi_slow += o_s * (0.82f + 0.18f * nz);
    while (phi_slow > two_pi * 8.f) {
        phi_slow -= two_pi * 8.f;
    }

    c->phi = phi;
    c->phi2 = phi2;
    c->phi_slow = phi_slow;
    c->nz = nz;

    const float a = 0.5f + 0.5f * sinf(phi2);
    const float b = 0.5f + 0.5f * sinf(phi2 * RadioConfig::analyzMouthChompHarm + phi_slow);
    const float chomp_s = sqrtf(fmaxf(0.f, a * b));
    const float cf = fminf(0.98f, fmaxf(0.f, RadioConfig::analyzMouthChompFloor));
    c->chomp = cf + (1.f - cf) * chomp_s;

    c->W = RadioConfig::analyzWidth;
    c->L = (int)RadioConfig::analyzMouthEdgeCols;
    const int row_off = (int)RadioConfig::analyzWaveRowOffset;
    c->u_fix = constrain((int)RadioConfig::analyzMouthEdgeUpperRow + row_off, 0, 7);
    c->l_fix = constrain((int)RadioConfig::analyzMouthEdgeLowerRow + row_off, 0, 7);

    const float extra_base = RadioConfig::analyzMouthHalfSepMin +
                             v * (RadioConfig::analyzMouthHalfSepMax - RadioConfig::analyzMouthHalfSepMin);
    c->lip_wobble = 0.5f + 0.5f * sinf(phi_slow * 1.47f + phi * 1.9f + nz * 4.f);
    c->extra_open = extra_base * c->chomp * (0.74f + 0.26f * c->lip_wobble);
    c->ripple = fminf(0.35f, fmaxf(0.f, RadioConfig::analyzMouthMaskRipple));
    c->bob =
        RadioConfig::analyzMouthAnchorNoBob
            ? 0.f
            : (RadioConfig::analyzMouthBobAmp * fmaxf(0.35f, v) * sinf(phi));
    c->kk = fmaxf(0.15f, RadioConfig::analyzMouthHyperK);
    c->sep_base = 0.5f * (float)(c->l_fix - c->u_fix);
    c->mid = 0.5f * (float)(c->u_fix + c->l_fix) + c->bob;
    c->min_gap = (int)RadioConfig::analyzMouthMinPixelGap;
    c->iw = c->W - 2 * c->L;
    c->curve_kind = RadioConfig::analyzMouthCurveKind;
}

static void mouth_robot_compute_up_lo(const MouthRobotCtx* c, int8_t up[32], int8_t lo[32]) {
    const int W = c->W;
    const int L = c->L;
    const int u_fix = c->u_fix;
    const int l_fix = c->l_fix;
    const int min_gap = c->min_gap;
    const int iw = c->iw;
    const float kk = c->kk;
    const float extra_open = c->extra_open;
    const float ripple = c->ripple;
    const float mid = c->mid;
    const float sep_base = c->sep_base;
    const float phi2 = c->phi2;
    const float phi_slow = c->phi_slow;

    for (int i = 0; i < W; i++) {
        if (L > 0 && (i < L || i >= W - L)) {
            int u = u_fix;
            int l = l_fix;
            if (min_gap > 0 && l < u + min_gap) {
                l = u + min_gap;
                if (l > 7) {
                    l = 7;
                    u = l - min_gap;
                    if (u < 0) {
                        u = 0;
                        l = min_gap > 7 ? 7 : min_gap;
                    }
                }
            }
            up[i] = (int8_t)u;
            lo[i] = (int8_t)l;
            continue;
        }

        float t = 0.f;
        if (iw > 1) {
            t = 2.f * (float)(i - L) / (float)(iw - 1) - 1.f;
        }
        float open_mask;
        if (c->curve_kind != 0) {
            const float raw = 1.f / (1.f + kk * t * t);
            const float r_edge = 1.f / (1.f + kk);
            open_mask = (raw - r_edge) / fmaxf(1e-4f, 1.f - r_edge);
        } else {
            open_mask = 1.f - t * t;
        }
        if (open_mask < 0.f) {
            open_mask = 0.f;
        } else if (open_mask > 1.f) {
            open_mask = 1.f;
        }
        if (ripple > 0.f && open_mask > 0.f) {
            const float rip = 0.5f + 0.5f * sinf(phi2 * 2.71f + phi_slow * 0.89f + (float)i * 0.51f);
            open_mask *= 1.f - ripple + ripple * rip;
            if (open_mask < 0.f) {
                open_mask = 0.f;
            } else if (open_mask > 1.f) {
                open_mask = 1.f;
            }
        }
        const float sep = sep_base + extra_open * open_mask;
        const float yu = mid - sep;
        const float yl = mid + sep;
        int u = constrain((int)roundf(yu), 0, 7);
        int l = constrain((int)roundf(yl), 0, 7);
        if (min_gap > 0 && l < u + min_gap) {
            l = u + min_gap;
            if (l > 7) {
                l = 7;
                u = l - min_gap;
                if (u < 0) {
                    u = 0;
                    l = min_gap > 7 ? 7 : min_gap;
                }
            }
        }
        up[i] = (int8_t)u;
        lo[i] = (int8_t)l;
    }
}

static void mouth_robot_draw_lips(int W, const int8_t* up, const int8_t* lo, bool invert) {
    if (W <= 1) {
        mtrx.dot(0, (uint8_t)up[0], mouth_gfx_on(invert));
        mtrx.dot(0, (uint8_t)lo[0], mouth_gfx_on(invert));
        return;
    }
    for (int i = 0; i < W - 1; i++) {
        mtrx.line(i, (int)up[i], i + 1, (int)up[i + 1], mouth_gfx_on(invert));
        mtrx.line(i, (int)lo[i], i + 1, (int)lo[i + 1], mouth_gfx_on(invert));
    }
}

static void mouth_robot_one_frame(uint8_t vol, bool invert) {
    MouthRobotCtx ctx;
    mouth_robot_fill_ctx(vol, &ctx);
    int8_t up[32];
    int8_t lo[32];
    mouth_robot_compute_up_lo(&ctx, up, lo);
    mouth_robot_draw_lips(ctx.W, up, lo, invert);
}

}  // namespace

// Режим 4/5 (у прошивці 3/4): «рот робота» — invert задається з switch.
void analyz_mouth_robot_backup(uint8_t vol, bool invert) {
    mouth_robot_one_frame(vol, invert);
}

// ========================= SYSTEM =========================
static uint32_t s_wifi_last_activity_ms = 0;

void audio_showstreamtitle(const char* info) {
}

void wifi_touch_activity() {
    s_wifi_last_activity_ms = millis();
}

void syncWifiWithAudioSilence() {
    if (!RadioConfig::wifiSleepWhenSilent) {
        return;
    }
    const bool wifiIdleLong =
        (RadioConfig::wifiIdleSleepAfterMs > 0) &&
        ((uint32_t)(millis() - s_wifi_last_activity_ms) >= RadioConfig::wifiIdleSleepAfterMs);
    WiFi.setSleep(wifiIdleLong);
    if (RadioConfig::wifiPsMaxModemWhenSilent && WiFi.getMode() != WIFI_OFF) {
        esp_wifi_set_ps(wifiIdleLong ? WIFI_PS_MAX_MODEM : WIFI_PS_NONE);
    }
}

void wifi_ap_toggle_from_core0() {
    if (wifiConnecting) {
        return;
    }
    WifiStored w;
    nvsLoadWifi(w);
    const String apSsid = nvsEffectiveApSsid(w);
    const String apPwd = nvsEffectiveApPass(w);

    const bool sta_ok = (WiFi.status() == WL_CONNECTED);
    const wifi_mode_t mode = WiFi.getMode();
    const bool ap_mode = (mode == WIFI_AP_STA || mode == WIFI_AP);

    if (!sta_ok && ap_mode) {
        return;
    }
    if (sta_ok && ap_mode) {
        WiFi.softAPdisconnect(true);
        WiFi.mode(WIFI_STA);
        wifi_touch_activity();
        syncWifiWithAudioSilence();
        print_val('A', 0);
        return;
    }
    if (sta_ok && mode == WIFI_STA) {
        WiFi.mode(WIFI_AP_STA);
        if (apPwd.length() >= 8) {
            WiFi.softAP(apSsid.c_str(), apPwd.c_str());
        } else {
            WiFi.softAP(apSsid.c_str());
        }
        wifi_touch_activity();
        syncWifiWithAudioSilence();
        print_val('A', 1);
        return;
    }
    if (!sta_ok && mode == WIFI_STA) {
        WiFi.mode(WIFI_AP_STA);
        if (apPwd.length() >= 8) {
            WiFi.softAP(apSsid.c_str(), apPwd.c_str());
        } else {
            WiFi.softAP(apSsid.c_str());
        }
        wifi_touch_activity();
        syncWifiWithAudioSilence();
        print_val('A', 1);
    }
}

void core0(void* p) {
    // ========================= SETUP =========================
    EncButton eb(RadioConfig::encS1, RadioConfig::encS2, RadioConfig::encBtn);
    eb.setClickTimeout(480);
    Tmr viz_tmr(42);
    Tmr eye_tmr(150);
    Tmr matrix_tmr(1000);
    Tmr angry_tmr(800);
    Tmr pong_tmr(145);
    square_tmr.timerMode(1);
    matrix_tmr.timerMode(1);
    angry_tmr.timerMode(1);
    bool pulse = 0;
    uint8_t pcm_pulse_l = 0;
    static uint32_t enc_btn_press_ms = 0;

    EEPROM.begin(memory.blockSize());
    memory.begin(0, 'b');

    mtrx.begin();
    mtrx.setBright((uint8_t)0);
    mtrx.clear();
    mtrx.update();
    delay(RadioConfig::coldStartMatrixZeroMs);
    upd_bright();
    mtrx.update();

    delay(RadioConfig::coldStartAfterMatrixMs);

    audio.setBufsize(RadioConfig::radioBuffer, -1);
    audio.setPinout(RadioConfig::i2sBclk, RadioConfig::i2sLrc, RadioConfig::i2sDout);
    audio.setVolume(data.state ? data.vol : 0);
    data.station = constrain(data.station, 0, sizeof(stations) / sizeof(char*) - 1);
    reconnect = stations[data.station];

    battery_init();

    s_wifi_last_activity_ms = millis();
    if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT0 && RadioConfig::wakeAfterSleepAnimMs > 0) {
        s_wake_after_sleep_anim_until_ms = millis() + RadioConfig::wakeAfterSleepAnimMs;
        s_pending_change_state_after_wake = true;
    }
    syncWifiWithAudioSilence();

    // ========================= LOOP =========================
    for (;;) {
        battery_update();
        square_tmr.tick();
        matrix_tmr.tick();
        angry_tmr.tick();
        memory.tick();

        if (s_pending_change_state_after_wake) {
            if ((int32_t)(millis() - s_wake_after_sleep_anim_until_ms) >= 0) {
                s_pending_change_state_after_wake = false;
                s_wake_after_sleep_anim_until_ms = 0;
                change_state();
            }
        }
        const bool show_wake_after_sleep_anim =
            s_pending_change_state_after_wake &&
            (int32_t)(millis() - s_wake_after_sleep_anim_until_ms) < 0;

        const bool eb_tick = eb.tick();
        if (data.state) {
            s_wifi_last_activity_ms = millis();
        } else if (eb_tick && (eb.press() || eb.release() || eb.turn())) {
            s_wifi_last_activity_ms = millis();
        }
        if (eb_tick && eb.press()) {
            enc_btn_press_ms = millis();
        }
        if (eb_tick && eb.pressing() && eb.pressFor() >= RadioConfig::encoderHardResetHoldMs) {
            ESP.restart();
        }
        if (eb_tick && eb.release()) {
            const uint32_t dur = millis() - enc_btn_press_ms;
            if (dur >= RadioConfig::encoderSleepHoldMs && dur < RadioConfig::encoderHardResetHoldMs) {
                memory.update();
                if (data.state) {
                    audio.setVolume(0);
                    audio.stopSong();
                }
                {
                    uint8_t br_off[] = {0, 0, 0, 0, 0};
                    mtrx.setBright(br_off);
                    mtrx.clear();
                    mtrx.update();
                }
                // ext0: RTC GPIO encBtn, пробуждение при нажатии кнопки (LOW к GND, подтяжка вверх).
                esp_sleep_enable_ext0_wakeup((gpio_num_t)RadioConfig::encBtn, 0);
                esp_deep_sleep_start();
            }
        }
        const bool eb_e = (!wifiConnecting && !show_wake_after_sleep_anim && eb_tick);

        // Только core0 трогает MAX7219: вызов anim_search с core1 в setup() давал гонку и мигание при Wi‑Fi.
        if (wifiConnecting || show_wake_after_sleep_anim) {
            anim_search();
        } else if (pong_active()) {
            if (pong_tmr.tick()) {
                pong_step();
                (void)pong_goal_fx_active();
                pong_draw();
                draw_eyes_follow_ball(pong_ball_x(), pong_ball_y());
                pong_sync_matrix_brightness();
                mtrx.update();
            }
            if (eb_e) {
                if (eb.turn() && !eb.pressing()) {
                    pong_paddle_nudge(eb.dir());
                    pong_draw();
                    draw_eyes_follow_ball(pong_ball_x(), pong_ball_y());
                    pong_sync_matrix_brightness();
                    mtrx.update();
                }
                if (eb.hasClicks()) {
                    const uint8_t n = eb.getClicks();
                    if (n == 1 && pong_serve_waiting()) {
                        pong_resume_after_goal();
                        pong_draw();
                        draw_eyes_follow_ball(pong_ball_x(), pong_ball_y());
                        pong_sync_matrix_brightness();
                        mtrx.update();
                    } else if (n == 5) {
                        pong_set_active(false);
                        upd_bright();
                        mtrx.update();
                    } else if (n == 6) {
                        wifi_ap_toggle_from_core0();
                        matrix_tmr.start(RadioConfig::matrixOverlayDigitsMs);
                    }
                }
                memory.update();
            }
        } else {
            if (data.state && !square_tmr.state()) {
                if (eye_tmr) {
                    draw_eye(0);
                    draw_eye(1);
                    if (angry_tmr.state()) {
                        draw_eyeb(0, 3, 3);
                        draw_eyeb(1, 3, 3);
                        mtrx.lineH(0, RadioConfig::analyzWidth, RadioConfig::analyzWidth + 16 - 1, GFX_CLEAR);
                        mtrx.lineH(1, RadioConfig::analyzWidth + 5, RadioConfig::analyzWidth + 5 + 6 - 1, GFX_CLEAR);
                        mtrx.lineH(2, RadioConfig::analyzWidth + 6, RadioConfig::analyzWidth + 6 + 4 - 1, GFX_CLEAR);
                        mtrx.lineH(3, RadioConfig::analyzWidth + 7, RadioConfig::analyzWidth + 7 + 2 - 1, GFX_CLEAR);
                    } else {
                        if (eb.pressing()) {
                            draw_eyeb(0, 4, 3, 3);
                            draw_eyeb(1, 1, 3, 3);
                        } else {
                            static uint16_t pos;
                            pos += 15;
                            uint8_t x = inoise8(pos);
                            uint8_t y = inoise8(pos + UINT16_MAX / 4);
                            x = constrain(x, 40, 255 - 40);
                            y = constrain(y, 40, 255 - 40);
                            x = map(x, 40, 255 - 40, 2, 5);
                            y = map(y, 40, 255 - 40, 2, 5);
                            if (pulse) {
                                pulse = 0;
                                int8_t sx = random(-1, 1);
                                int8_t sy = random(-1, 1);
                                draw_eyeb(0, x + sx, y + sy, 3);
                                draw_eyeb(1, x + sx, y + sy, 3);
                            } else {
                                draw_eyeb(0, x, y);
                                draw_eyeb(1, x, y);
                            }
                        }
                    }
                    mtrx.update();
                }
            }

            // Режимы рта 0…4: волна / волна инв. / EQ / рот / рот инв.
            if (data.mode > 4) {
                data.mode = 0;
            }
            if (viz_tmr && !matrix_tmr.state() && (data.state || data.mode <= 4)) {
                const uint8_t vol = pcm_vis_after_noise_gate(g_pcm_vis);
                if (vol > pcm_pulse_l + 12) {
                    pulse = 1;
                }
                pcm_pulse_l = (uint8_t)((pcm_pulse_l * 3u + vol) / 4u);

                const bool mouth_invert = (data.mode == 1 || data.mode == 4);
                mtrx.rect(0, 0, RadioConfig::analyzWidth - 1, 7, mouth_gfx_off(mouth_invert));
                const uint8_t v_mouth = pcm_wave_level_after_gate();
                switch (data.mode) {
                    case 0:
                        analyz0(v_mouth, false);
                        break;
                    case 1:
                        analyz0(v_mouth, true);
                        break;
                    case 2:
                        analyz_eq_bars(v_mouth, false);
                        break;
                    case 3:
                        analyz_mouth_robot_backup(v_mouth, false);
                        break;
                    case 4:
                        analyz_mouth_robot_backup(v_mouth, true);
                        break;
                    default:
                        data.mode = 0;
                        analyz0(v_mouth, false);
                        break;
                }
                mtrx.update();
            }

            if (eb_e) {
                static bool station_changed = 0;

                if (eb.turn()) {
                    if (eb.pressing()) {
                        // getClicks() при удержании = число уже завершённых кликов в серии:
                        // 0 — первое нажатие; 1 — второе нажатие (двойной клик + поворот); 2 — третье (тройной + поворот).
                        switch (eb.getClicks()) {
                            case 0:
                                data.station += eb.dir();
                                data.station = constrain(data.station, 0, sizeof(stations) / sizeof(char*) - 1);
                                print_val('s', data.station);
                                matrix_tmr.start(RadioConfig::matrixOverlayDigitsMs);
                                station_changed = 1;
                                break;
                            case 1: {
                                const int8_t d = eb.dir();
                                int m = (int)data.mode + (int)d;
                                m = (m % 5 + 5) % 5;
                                data.mode = (uint8_t)m;
                                break;
                            }
                            case 2: {
                                const int8_t d = eb.dir();
                                data.bright_mouth += d;
                                data.bright_eyes += d;
                                data.bright_mouth = constrain(data.bright_mouth, 0, 16);
                                data.bright_eyes = constrain(data.bright_eyes, 0, 16);
                                upd_bright();
                                break;
                            }
                            default:
                                break;
                        }
                    } else {
                        if (data.state) {
                            angry_tmr.start();
                            data.vol += eb.dir();
                            data.vol = constrain(data.vol, 0, 21);
                            audio.setVolume(data.vol);
                            syncWifiWithAudioSilence();
                            print_val('v', data.vol);
                            matrix_tmr.start(RadioConfig::matrixOverlayDigitsMs);
                        }
                    }
                }

                if (eb.hasClicks()) {
                    switch (eb.getClicks()) {
                        case 1:
                            data.state = !data.state;
                            if (!data.state) {
                                audio.setVolume(0);
                                audio.stopSong();
                            } else {
                                reconnect = stations[data.station];
                                audio.setVolume(data.vol);
                            }
                            syncWifiWithAudioSilence();
                            change_state();
                            break;
                        case 2:
                            break;
                        case 3:
                            data.trsh = (uint16_t)constrain((int)g_pcm_level_adc * 2 / 3, 4, 3800);
                            break;
                        case 4:
                            if (RadioConfig::batteryMonitorEnable) {
                                print_batt(battery_percent());
                                matrix_tmr.start((uint16_t)RadioConfig::batteryPercentShowDurationMs);
                            }
                            break;
                        case 5:
                            pong_start();
                            pong_tmr.start();
                            pong_draw();
                            draw_eyes_follow_ball(pong_ball_x(), pong_ball_y());
                            pong_sync_matrix_brightness();
                            mtrx.update();
                            break;
                        case 6:
                            wifi_ap_toggle_from_core0();
                            matrix_tmr.start(RadioConfig::matrixOverlayDigitsMs);
                            break;
                    }
                }

                if (eb.release()) {
                    if (station_changed) {
                        station_changed = 0;
                        reconnect = stations[data.station];
                    }
                }
                memory.update();
            }
        }

        syncWifiWithAudioSilence();

        if (RadioConfig::core0LoopDelayMs > 0) {
            delay(RadioConfig::core0LoopDelayMs);
        }
        TIMERG0.wdt_wprotect = TIMG_WDT_WKEY_VALUE;  // write enable
        TIMERG0.wdt_feed = 1;                        // feed dog
        TIMERG0.wdt_wprotect = 0;                    // write protect
    }
}