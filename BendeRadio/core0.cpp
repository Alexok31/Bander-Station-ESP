#include "core0.h"

#include <cstring>
#include <atomic>
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
#include "battery_matrix.h"
#include "AirPlay.h"
#include "AudioMux.h"
#include "BtAudio.h"
#include "NvsConfig.h"
#include "pong.h"
#include "tmr.h"
#include "BenderAi.h"
#include "mpu6050.h"
#include "secrets.h"

static inline uint8_t mouth_gfx_on(bool invert) {
    return invert ? GFX_CLEAR : GFX_FILL;
}
static inline uint8_t mouth_gfx_off(bool invert) {
    return invert ? GFX_FILL : GFX_CLEAR;
}

static uint32_t s_face_last_live_ms = 0;
static bool s_wake_on_shake = true;
static uint32_t s_face_calm_after_ms = RadioConfig::benderFaceCalmAfterMs;
static bool matrix_face_awake();
static BenderLife s_life;
static BenderEvent s_life_face = BenderEvent::None;
static uint32_t s_life_face_since = 0;

static void react_to_life(BenderEvent event) {
    if (event == BenderEvent::None || bender_ai_busy() || pong_active()) return;
    if (!bender_event_allows_motion_wake(event, matrix_face_awake(), s_wake_on_shake)) return;
    const uint32_t now = millis();
    s_life_face = event;
    s_life_face_since = now;
    s_face_last_live_ms = now;
    Serial.printf("[Life] %s\n", bender_event_name(event));
    // Direct physical interactions can speak while radio playback is paused.
    if (!bender_event_radio_allows_voice(event, radioState.state) ||
        strcmp(g_audio_source, "wifi") != 0 || airplay_owns_speaker() ||
        radioState.vol <= 0 || WiFi.status() != WL_CONNECTED || !bender_ai_event_voice_enabled()) return;
    if (s_life.allowVoice(event, now, false) && bender_ai_event(event, (int)radioState.station))
        s_life.allowVoice(event, now);
}

static bool matrix_face_awake() {
    if (radioState.state || bender_ai_busy() || airplay_playing()) {
        s_face_last_live_ms = millis();
        return true;
    }
    return BenderBehavior::faceAwake(millis(), s_face_last_live_ms, s_face_calm_after_ms);
}

// Встроенные станции (0…1). Дополнительные — только через Web UI → NVS.
const char* stations[] = {
    "https://uk3.internet-radio.com/proxy/majesticjukebox?mp=/live",  // 0 Majestic Jukebox
    "http://stream81.metacast.eu/radio1rock128",                      // 1 Radio1 Rock
};
static constexpr uint8_t kStationBuiltInCount = (uint8_t)(sizeof(stations) / sizeof(stations[0]));
static String s_custom_stations[RadioConfig::customStationMaxCount];
static uint8_t s_custom_station_count = 0;
static int8_t s_matrix_brightness_trim[RadioConfig::matrixModuleCount] = {0, 0, 0, 0, 0};
static volatile bool s_matrix_brightness_trim_dirty = false;
extern Data radioState;

static uint8_t station_total_count() {
    return (uint8_t)(kStationBuiltInCount + s_custom_station_count);
}

static const char* station_url_by_index(int idx) {
    if (idx < 0) {
        return stations[0];
    }
    if (idx < (int)kStationBuiltInCount) {
        return stations[(uint8_t)idx];
    }
    const int custom_idx = idx - (int)kStationBuiltInCount;
    if (custom_idx >= 0 && custom_idx < (int)s_custom_station_count) {
        return s_custom_stations[(uint8_t)custom_idx].c_str();
    }
    return stations[0];
}

static void station_clamp_index();

const char* station_url_for_current() {
    return station_url_by_index(radioState.station);
}

uint8_t radio_station_count() {
    return station_total_count();
}

const char* radio_station_name(uint8_t idx) {
    static const char* kBuiltInNames[] = {
        "Majestic Jukebox",
        "Radio1 Rock",
    };
    static char custom_label[40];
    const uint8_t n_built = (uint8_t)(sizeof(kBuiltInNames) / sizeof(kBuiltInNames[0]));
    if (idx < n_built && idx < kStationBuiltInCount) {
        return kBuiltInNames[idx];
    }
    snprintf(custom_label, sizeof(custom_label), "станція %u", (unsigned)idx);
    return custom_label;
}

void radio_voice_set_state(bool on, int station) {
    if (station >= 0) {
        radioState.station = (int8_t)station;
        station_clamp_index();
    }
    radioState.state = on;
    Serial.printf("[Radio] voice %s station %d\n", on ? "ON" : "OFF", (int)radioState.station);
}

void radio_voice_after_speaker() {
    if (strcmp(g_audio_source, "bt") == 0) {
        if (radioState.state) {
            bt_audio_avrcp_play();
        } else {
            bt_audio_avrcp_pause();
        }
    } else if (!radioState.state) {
        audio.setVolume(0);
        audio.stopSong();
    }
    apply_output_volume();
    syncWifiWithAudioSilence();
    change_state();
}

static void station_clamp_index() {
    const int total = (int)station_total_count();
    if (total <= 0) {
        radioState.station = 0;
        return;
    }
    radioState.station = constrain(radioState.station, 0, total - 1);
}

static void radio_step_station(int dir) {
    const int n = (int)station_total_count();
    if (n <= 0) {
        radioState.station = 0;
        return;
    }
    int i = (int)radioState.station + dir;
    if (i < 0) {
        i = n - 1;
    } else if (i >= n) {
        i = 0;
    }
    radioState.station = (int8_t)i;
}

// data
MAX7219<5, 1, RadioConfig::mtrxCs, RadioConfig::mtrxDat, RadioConfig::mtrxClk> mtrx;
Data radioState;
EEManager memory(radioState);
Audio audio;
String streamname;
const char* reconnect = nullptr;
volatile bool wifiConnecting = false;
static uint32_t s_sta_connect_started_ms = 0;
static bool s_sta_softap_on_fail = true;
static bool s_sta_keep_ap = false;
static uint32_t s_ap_sta_retry_ms = 0;

static uint32_t s_wake_after_sleep_anim_until_ms = 0;
static bool s_pending_change_state_after_wake = false;

// Время millis(), с которого разрешена подсветка и отрисовка (после matrixDisplayEnableDelayMs).
static uint32_t g_matrix_display_enable_ms = 0xFFFFFFFFu;
static bool s_matrix_ui_started = false;

// Выбор Wi‑Fi / Bluetooth: 4×клик + удержание + поворот — текст на рту; применение при отпускании кнопки.
static bool s_mode_pick_active = false;
static char s_mode_pick_choice[8] = "radio";
static volatile bool s_mode_flash_req = false;

static inline bool matrix_display_ready() {
    if (g_matrix_display_enable_ms == 0xFFFFFFFFu) {
        return false;
    }
    return (int32_t)(millis() - g_matrix_display_enable_ms) >= 0;
}

static uint32_t s_matrix_last_flush_ms = 0;

static void matrix_flush(bool force = false) {
    const uint32_t now = millis();
    if (!force) {
        if (!matrix_display_ready()) {
            return;
        }
        if (RadioConfig::matrixUpdateMinIntervalMs > 0 &&
            (uint32_t)(now - s_matrix_last_flush_ms) < (uint32_t)RadioConfig::matrixUpdateMinIntervalMs) {
            return;
        }
    }
    s_matrix_last_flush_ms = now;
    mtrx.update();
}

// func
// ========================= MATRIX =========================
static void matrix_apply_brightness(int base) {
    uint8_t br[RadioConfig::matrixModuleCount];
    for (uint8_t i = 0; i < RadioConfig::matrixModuleCount; i++) {
        const int v = constrain(base + (int)s_matrix_brightness_trim[i], 0, 15);
        br[i] = (uint8_t)v;
    }
    mtrx.setBright(br);
}

static int matrix_max_offset() {
    int m = 0;
    for (uint8_t i = 0; i < RadioConfig::matrixModuleCount; i++) {
        if ((int)s_matrix_brightness_trim[i] > m) {
            m = (int)s_matrix_brightness_trim[i];
        }
    }
    return m;
}

static int matrix_base_max() {
    const int v = 15 - matrix_max_offset();
    return (v < 0) ? 0 : v;
}

void upd_bright() {
    // One selected level in every state, including idle and AI waiting/speech.
    int v = max((int)radioState.bright_mouth, (int)radioState.bright_eyes);
    v = constrain(v, 0, matrix_base_max());
    if (RadioConfig::matrixBrightnessWhenPlayingCap < 15) {
        v = min(v, (int)RadioConfig::matrixBrightnessWhenPlayingCap);
    }
    radioState.bright_mouth = (int8_t)v;
    radioState.bright_eyes = (int8_t)v;
    matrix_apply_brightness(v);
}

uint8_t matrix_get_base_brightness() {
    const int v = constrain(max((int)radioState.bright_mouth, (int)radioState.bright_eyes), 0, matrix_base_max());
    return (uint8_t)v;
}

void matrix_get_brightness_trim(int8_t* outTrim, uint8_t count) {
    if (outTrim == nullptr || count == 0) {
        return;
    }
    const uint8_t n = (count < RadioConfig::matrixModuleCount) ? count : RadioConfig::matrixModuleCount;
    for (uint8_t i = 0; i < n; i++) {
        outTrim[i] = s_matrix_brightness_trim[i];
    }
}

void matrix_set_brightness_trim(const int8_t* trim, uint8_t count, bool persist) {
    if (trim == nullptr || count == 0) {
        return;
    }
    const uint8_t n = (count < RadioConfig::matrixModuleCount) ? count : RadioConfig::matrixModuleCount;
    for (uint8_t i = 0; i < n; i++) {
        s_matrix_brightness_trim[i] =
            (int8_t)constrain((int)trim[i], (int)RadioConfig::matrixBrightnessTrimMin,
                              (int)RadioConfig::matrixBrightnessTrimMax);
    }
    if (persist) {
        nvsSaveMatrixBrightnessTrim(s_matrix_brightness_trim, RadioConfig::matrixModuleCount);
    }
    s_matrix_brightness_trim_dirty = true;
}

// Глиф 5×7 внутри одной 8×8-клетки (модуль MAX7219): строка = 5 бит, старший бит — левый столбец.
static void draw_mode_pick_glyph_cell(uint8_t cell, const uint8_t rows[7]) {
    const uint8_t x0 = (uint8_t)(cell * 8u + 1u);
    for (uint8_t y = 0; y < 7u; y++) {
        const uint8_t bits = rows[y];
        for (uint8_t c = 0; c < 5u; c++) {
            if ((bits >> (4u - c)) & 1u) {
                mtrx.dot(x0 + c, y + 1, GFX_FILL);
            }
        }
    }
}

// Рот: «FM» / «AIR» / «wfi» / «bt»
static void draw_mode_pick_mouth() {
    mtrx.rect(0, 0, RadioConfig::analyzWidth - 1, 7, GFX_CLEAR);
    static const uint8_t gW[7] = {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A};
    static const uint8_t gF[7] = {0x1E, 0x10, 0x10, 0x1C, 0x10, 0x10, 0x10};
    static const uint8_t gI[7] = {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x1F};
    static const uint8_t gB[7] = {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E};
    static const uint8_t gT[7] = {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04};
    static const uint8_t gA[7] = {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11};
    static const uint8_t gM[7] = {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11};
    static const uint8_t gR[7] = {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11};

    if (strcmp(s_mode_pick_choice, "bt") == 0) {
        draw_mode_pick_glyph_cell(0, gB);
        draw_mode_pick_glyph_cell(1, gT);
    } else if (strcmp(s_mode_pick_choice, "ap") == 0) {
        draw_mode_pick_glyph_cell(0, gA);
        draw_mode_pick_glyph_cell(1, gI);
        draw_mode_pick_glyph_cell(2, gR);
    } else if (strcmp(s_mode_pick_choice, "wifi") == 0) {
        draw_mode_pick_glyph_cell(0, gW);
        draw_mode_pick_glyph_cell(1, gF);
        draw_mode_pick_glyph_cell(2, gI);
    } else {
        draw_mode_pick_glyph_cell(0, gF);
        draw_mode_pick_glyph_cell(1, gM);
    }
}

static void draw_current_play_mode_mouth() {
    strncpy(s_mode_pick_choice, play_mode_is_airplay() ? "ap" : "radio", sizeof(s_mode_pick_choice) - 1);
    s_mode_pick_choice[sizeof(s_mode_pick_choice) - 1] = '\0';
    draw_mode_pick_mouth();
}

void matrix_show_play_mode() {
    s_mode_flash_req = true;
}

static void pong_sync_matrix_brightness() {
    upd_bright();
}
static std::atomic<int> s_ui_vol_pending{-1};

void matrix_show_volume(int8_t vol) {
    if (vol < 0) {
        vol = 0;
    }
    if (vol > 99) {
        vol = 99;
    }
    s_ui_vol_pending.store(vol);
}

void print_val(char c, uint8_t v) {
    if (!matrix_display_ready()) {
        return;
    }
    // Завжди тёмный фон + светлый шрифт: библиотечный print не умеет «тёмные» глифы при інверсії рота.
    mtrx.rect(0, 0, RadioConfig::analyzWidth - 1, 7, GFX_CLEAR);
    mtrx.setCursor(8 * 0 + 2, 1);
    mtrx.print(c);
    mtrx.setCursor(8 * 1 + 2, 1);
    mtrx.print(v / 10);
    mtrx.setCursor(8 * 2 + 2, 1);
    mtrx.print(v % 10);
    matrix_flush();
}

static void draw_batt_matrix_rows(const uint8_t rows[8]) {
    for (int y = 0; y < 8; y++) {
        const uint8_t b = rows[(uint8_t)y];
        for (int x = 0; x < 8; x++) {
            if (b & (uint8_t)(1 << (7 - x))) {
                mtrx.dot((uint8_t)x, (uint8_t)y, GFX_FILL);
            }
        }
    }
}

static bool s_batt_matrix_overlay;
static uint8_t s_batt_matrix_overlay_pct;
static uint32_t s_batt_icon_step_ms;
static uint32_t s_batt_charge_frame;
static bool s_batt_overlay_prev_chg;

static void print_batt_overlay(uint8_t pct) {
    if (!matrix_display_ready()) {
        return;
    }
    const uint8_t v = (pct > 99u) ? 99u : pct;
    mtrx.rect(0, 0, RadioConfig::analyzWidth - 1, 7, GFX_CLEAR);
    const bool chg = RadioConfig::chargingDetectEnable && battery_is_charging();
    uint8_t rows[8];
    if (chg) {
        battery_matrix_rows_charging(v, s_batt_charge_frame, rows);
    } else {
        battery_matrix_rows_from_percent(v, rows);
    }
    draw_batt_matrix_rows(rows);
    mtrx.setCursor(8 * 1 + 2, 1);
    mtrx.print(battery_gauge_ready() ? (char)('0' + (v / 10)) : '-');
    mtrx.setCursor(8 * 2 + 2, 1);
    mtrx.print(battery_gauge_ready() ? (char)('0' + (v % 10)) : '-');
    matrix_flush();
}

void print_batt(uint8_t pct) {
    print_batt_overlay(pct);
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

// Радио выкл.: статичные «спящие» глаза (тот же вид, что в change_state при !radioState.state).
static void draw_eyes_radio_idle_off() {
    draw_eye(0);
    draw_eye(1);
    mtrx.rect(RadioConfig::analyzWidth, 0, RadioConfig::analyzWidth + 16 - 1, 3, GFX_CLEAR);
    draw_eyeb(0, 3, 5);
    draw_eyeb(1, 3, 5);
}

// Called only by core0. Clear the eye area so lids from the previous expression
// cannot leak into the next one. Mouth overlays (volume/battery) remain separate.
static void draw_ai_eyes(BenderFaceState state, uint32_t elapsed) {
    const int x0 = RadioConfig::analyzWidth;
    mtrx.rect(x0, 0, x0 + 15, 7, GFX_CLEAR);
    for (uint8_t i = 0; i < 2; ++i) {
        draw_eye(i);
        const int x = x0 + i * 8;
        if (state == BenderFaceState::Listening) {
            // Wide, attentive eyes. One short blink, not a constant flashing cue.
            draw_eyeb(i, 3, 3, 3);
            if (elapsed % 3200u > 3050u) {
                mtrx.rect(x, 0, x + 7, 2, GFX_CLEAR);
                mtrx.rect(x, 5, x + 7, 7, GFX_CLEAR);
            }
        } else if (state == BenderFaceState::Thinking) {
            const int gaze = ((elapsed / 900u) % 2u) ? 4 : 2;
            draw_eyeb(i, gaze, 1);
            mtrx.lineH(7, x, x + 7, GFX_CLEAR);
        } else if (state == BenderFaceState::Error) {
            // Asymmetric lids and a small sideways glance: puzzled/annoyed.
            mtrx.rect(x, 0, x + 7, i == 0 ? 2 : 1, GFX_CLEAR);
            draw_eyeb(i, elapsed < 700u ? 2 : 4, 4);
        } else if (state == BenderFaceState::Annoyed) {
            // Symmetric frown, without the asymmetric error/drunk expression.
            draw_eyeb(i, 3, 4);
            mtrx.lineH(0, x, x + 7, GFX_CLEAR);
            if (i == 0) {
                mtrx.lineH(1, x + 3, x + 7, GFX_CLEAR);
                mtrx.lineH(2, x + 5, x + 7, GFX_CLEAR);
            } else {
                mtrx.lineH(1, x, x + 4, GFX_CLEAR);
                mtrx.lineH(2, x, x + 2, GFX_CLEAR);
            }
        } else if (state == BenderFaceState::Pleased) {
            draw_eyeb(i, 3, 3);
            mtrx.lineH(7, x, x + 7, GFX_CLEAR);
            mtrx.lineH(6, x, x + 1, GFX_CLEAR);
            mtrx.lineH(6, x + 6, x + 7, GFX_CLEAR);
        } else if (state == BenderFaceState::Curious) {
            draw_eyeb(i, elapsed < 900 ? 2 : 4, 2);
        } else if (state == BenderFaceState::Tired) {
            mtrx.rect(x, 0, x + 7, 2, GFX_CLEAR);
            draw_eyeb(i, 3, 5);
        } else {
            draw_eyeb(i, ((elapsed / 1100u) % 2u) ? 3 : 2, 3);
        }
    }
}

static void draw_ai_error_mouth() {
    mtrx.rect(0, 0, RadioConfig::analyzWidth - 1, 7, GFX_CLEAR);
    const int mid = RadioConfig::analyzWidth / 2;
    mtrx.lineH(4, mid - 7, mid + 2);
    mtrx.lineH(3, mid + 3, mid + 6);
}

// Грустные глаза: uint8_t IMAGES[][8] пользователя → uint64 (младший байт = верхняя строка кадра для blit).
// Левый: 0x00,0x1e,0x3f,0x7f,0xff,0xff,0x7e,0x3c
static const uint64_t kBatterySadEyeFramesLeft[] = {
    0x0000000000000000ULL,
    0x3C7EFFFF7F3F1E00ULL,
    0x3C7EFFFF7F3F1E00ULL,
    0x3C7EFFFF7F3F1E00ULL,
    0x3C7EFFFF7F3F1E00ULL,
    0x3C7EFFFF7F3F1E00ULL,
    0x3C7EFFFF7F3F1E00ULL,
    0x3C7EFFFF7F3F1E00ULL,
};
// Правый: 0x00,0x78,0xfc,0xfe,0xff,0xff,0x7e,0x3c
static const uint64_t kBatterySadEyeFramesRight[] = {
    0x0000000000000000ULL,
    0x3C7EFFFFFEFC7800ULL,
    0x3C7EFFFFFEFC7800ULL,
    0x3C7EFFFFFEFC7800ULL,
    0x3C7EFFFFFEFC7800ULL,
    0x3C7EFFFFFEFC7800ULL,
    0x3C7EFFFFFEFC7800ULL,
    0x3C7EFFFFFEFC7800ULL,
};

static inline bool battery_sad_eyes_wanted() {
    return RadioConfig::batterySadEyesEnable && RadioConfig::batteryMonitorEnable && battery_gauge_ready() &&
           !battery_is_charging() && battery_percent() < RadioConfig::batterySadEyesBelowPercent;
}

static void blit_u64_8x8(uint8_t x0, uint8_t y0, uint64_t bits) {
    // 180°: физическая точка (r,c) = пиксель исходного кадра (7−r, 7−c).
    // В строке 0b… старший бит — левый столбец (как у IMAGES[][8]).
    for (uint8_t r = 0; r < 8; r++) {
        const uint8_t row = (uint8_t)((bits >> (56 - 8 * (uint64_t)(7 - r))) & 0xFFu);
        for (uint8_t c = 0; c < 8; c++) {
            const uint8_t on = ((row >> (7u - c)) & 1u) ? GFX_FILL : GFX_CLEAR;
            mtrx.dot(x0 + c, y0 + r, on);
        }
    }
}

static uint64_t battery_sad_frame_bits(uint8_t eye) {
    constexpr uint8_t kN = (uint8_t)(sizeof(kBatterySadEyeFramesLeft) / sizeof(kBatterySadEyeFramesLeft[0]));
    const uint8_t idx =
        (RadioConfig::batterySadEyesBitmapIndex < kN) ? RadioConfig::batterySadEyesBitmapIndex : (uint8_t)(kN - 1u);
    return (eye == 0u) ? kBatterySadEyeFramesLeft[idx] : kBatterySadEyeFramesRight[idx];
}

// Зрачок 2×2: центр (3,3); по горизонтали только ±1 от центра (±2 ломает обводку глаза на матрице).
// По вертикали до ±2 от центра; верхний левый (px,py) = (3+dx, 3+dy), clamp 1…5.
static constexpr int8_t kBattSadPupilCx = 3;
static constexpr int8_t kBattSadPupilCy = 3;
static int8_t s_batt_sad_dx = 0;
static int8_t s_batt_sad_dy = 0;
static uint32_t s_batt_sad_next_pupil_ms;
static bool s_batt_sad_was_active;
// После шага вниз на 1 (или диагонали с вниз) — следующий тик только «назад» по dy.
static bool s_batt_sad_pupil_need_return;

static void battery_sad_clamp_pupil_deltas() {
    if (s_batt_sad_dx < -1) {
        s_batt_sad_dx = -1;
    }
    if (s_batt_sad_dx > 1) {
        s_batt_sad_dx = 1;
    }
    if (s_batt_sad_dy < -2) {
        s_batt_sad_dy = -2;
    }
    if (s_batt_sad_dy > 2) {
        s_batt_sad_dy = 2;
    }
}

static void battery_sad_pupil_tick() {
    if (s_batt_sad_pupil_need_return) {
        s_batt_sad_dy--;
        battery_sad_clamp_pupil_deltas();
        s_batt_sad_pupil_need_return = false;
        return;
    }

    const int r = random(0, 100);
    if (r < 26) {
        // Ровно на 1 вниз от текущего положения (не ниже +2 от центра); потом возврат.
        if (s_batt_sad_dy < 2) {
            s_batt_sad_dy++;
            battery_sad_clamp_pupil_deltas();
            s_batt_sad_pupil_need_return = true;
        }
    } else if (r < 52) {
        // Влево или вправо на 1 (не дальше ±1 от центра).
        if (random(0, 2) != 0) {
            if (s_batt_sad_dx > -1) {
                s_batt_sad_dx--;
            }
        } else {
            if (s_batt_sad_dx < 1) {
                s_batt_sad_dx++;
            }
        }
        battery_sad_clamp_pupil_deltas();
    } else if (r < 90) {
        // Вниз на 1 и влево или вправо на 1; вертикаль на следующем тике отменяем.
        if (s_batt_sad_dy < 2) {
            s_batt_sad_dy++;
            if (random(0, 2) != 0) {
                if (s_batt_sad_dx > -1) {
                    s_batt_sad_dx--;
                }
            } else {
                if (s_batt_sad_dx < 1) {
                    s_batt_sad_dx++;
                }
            }
            battery_sad_clamp_pupil_deltas();
            s_batt_sad_pupil_need_return = true;
        }
    }
}

static void battery_sad_eyes_begin_session() {
    s_batt_sad_dx = 0;
    s_batt_sad_dy = 0;
    s_batt_sad_pupil_need_return = false;
    s_batt_sad_next_pupil_ms = millis() + RadioConfig::batterySadEyesPupilStepMs;
}

static void battery_sad_eyes_track_session() {
    const bool want = battery_sad_eyes_wanted();
    if (want && !s_batt_sad_was_active) {
        battery_sad_eyes_begin_session();
    }
    s_batt_sad_was_active = want;
}

static void battery_sad_maybe_tick_pupil() {
    const uint32_t now = millis();
    if ((int32_t)(now - s_batt_sad_next_pupil_ms) < 0) {
        return;
    }
    s_batt_sad_next_pupil_ms = now + RadioConfig::batterySadEyesPupilStepMs;
    battery_sad_pupil_tick();
}

static void draw_battery_sad_eyes_both() {
    battery_sad_eyes_track_session();
    battery_sad_maybe_tick_pupil();
    const uint8_t xL = RadioConfig::analyzWidth;
    const uint8_t xR = (uint8_t)(RadioConfig::analyzWidth + 8u);
    blit_u64_8x8(xL, 0, battery_sad_frame_bits(0));
    blit_u64_8x8(xR, 0, battery_sad_frame_bits(1));
    // Зрачок 2×2: центр (3,3) + (dx,dy); clamp 1…5 — отступ 1 px от края 8×8.
    const int px = constrain((int)kBattSadPupilCx + (int)s_batt_sad_dx, 1, 5);
    const int py = constrain((int)kBattSadPupilCy + (int)s_batt_sad_dy, 1, 5);
    draw_eyeb(0, px, py, 2);
    draw_eyeb(1, px, py, 2);
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

    if (!radioState.state) {
        draw_eyes_radio_idle_off();
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
    if (!matrix_display_ready()) {
        return;
    }
    static int8_t pos = 4, dir = 1;
    static Tmr tmr(50);
    if (tmr) {
        pos += dir;
        if (pos >= 6) dir = -1;
        if (pos <= 0) dir = 1;
        // Полный кадр: иначе после «спящих» глаз (change_state при !radioState.state, яркость 0) остаётся
        // старый рисунок слева/справа и смешивается с бегающими бровями (Wi‑Fi / BT / после сна).
        upd_bright();
        mtrx.clear();
        mtrx.rect(RadioConfig::analyzWidth, 2, RadioConfig::analyzWidth + 16 - 1, 5, GFX_FILL);
        draw_eyeb(0, pos, 3);
        draw_eyeb(1, pos, 3);
        matrix_flush();
    }
}

void change_state() {
    if (!matrix_display_ready()) {
        return;
    }
    mtrx.clear();
    if (radioState.state) {
        upd_bright();
        if (battery_sad_eyes_wanted()) {
            draw_battery_sad_eyes_both();
        } else {
            draw_eye(0);
            draw_eye(1);
            draw_eyeb(0, 2, 2, 4);
            draw_eyeb(1, 2, 2, 4);
        }
    } else {
        // Даже в «спящем» режиме используем общую яркость + per-module trim
        // (иначе калибровка глаз не видна).
        upd_bright();
        if (battery_sad_eyes_wanted()) {
            draw_battery_sad_eyes_both();
        } else {
            draw_eyes_radio_idle_off();
        }
    }
    matrix_flush();
}

// ========================= ANALYZ =========================
// g_pcm_level_adc в 0…4095 (inst*4095/100 в BendeRadio.ino). Порог тишины radioState.trsh — в тех же единицах.
// Раньше нарастание было +120 счётчиков ADC — при полной шкале 4095 это ~3% над порогом → «рот» почти всегда 1 px.
static uint16_t pcm_noise_gate_trsh_effective() {
    if (strcmp(g_audio_source, "bt") != 0) {
        return radioState.trsh;
    }
    const uint32_t t =
        (uint32_t)radioState.trsh * (uint32_t)RadioConfig::btPcmNoiseGateTrshPercent / 100u;
    if (t < 4u) {
        return 4u;
    }
    if (t > 3800u) {
        return 3800u;
    }
    return (uint16_t)t;
}

static uint8_t pcm_vis_after_noise_gate(uint8_t vw) {
    const uint16_t adc = g_pcm_level_adc;
    const uint16_t trsh = pcm_noise_gate_trsh_effective();
    if (adc <= trsh || vw == 0) {
        return 0;
    }
    if (RadioConfig::pcmNoiseGateBinary) {
        return vw;
    }
    const uint32_t above = (uint32_t)adc - (uint32_t)trsh;
    const uint32_t head = (uint32_t)RadioConfig::pcmLevelAdcMax - (uint32_t)trsh;
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
    if (bender_ai_tts_playing() || airplay_playing()) {
        return g_pcm_vis;
    }
    return pcm_vis_after_noise_gate(g_pcm_vis);
}

// Режим 1: колонки 1 px; фиксированные веса по X (две «горки» sin, произведение) — без бегущей фазы, движение только от PCM.
static void analyz_eq_bars(uint8_t v_gate, bool invert, bool rest = false) {
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
        const uint8_t raw = rest ? (uint8_t)100 : g_pcm_eq_band[col];
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

// Режим 5: бегущая строка (Gyver print) + таймлайн на нижнем ряду (BT / AIR).
static void analyz_bt_track_progress(bool invert) {
    const int W = RadioConfig::analyzWidth;
    static uint32_t s_bt_marquee_serial = 0xFFFFFFFFu;
    static int16_t s_bt_marquee_x = (int16_t)RadioConfig::analyzWidth;
    static uint32_t s_bt_marquee_adv_ms = 0;

    const bool ap = play_mode_is_airplay();
    const bool show_text = ap || strcmp(g_audio_source, "bt") == 0;
    if (show_text) {
        const uint32_t ser = ap ? airplay_track_meta_serial() : bt_audio_track_meta_serial();
        if (ser != s_bt_marquee_serial) {
            s_bt_marquee_serial = ser;
            s_bt_marquee_x = (int16_t)W;
        }
        const char* const line = ap ? airplay_track_scroll_cstr() : bt_audio_track_scroll_cstr();
        const uint32_t now_ms = millis();
        constexpr uint32_t kBtMarqueeMsPerPx = 84u;
        if ((uint32_t)(now_ms - s_bt_marquee_adv_ms) >= kBtMarqueeMsPerPx) {
            s_bt_marquee_adv_ms = now_ms;
            s_bt_marquee_x--;
        }
        const int text_px = (int)strlen(line) * 6 + W + 16;
        if (s_bt_marquee_x < -text_px) {
            s_bt_marquee_x = (int16_t)W;
        }
        mtrx.setScale(1);
        mtrx.invertText(invert);
        mtrx.setTextBound(0, W - 1);
        mtrx.setCursor((int)s_bt_marquee_x, 0);
        mtrx.print(line);
        mtrx.resetTextBound();
        mtrx.invertText(false);
    }

    const int y = 7;
    for (int x = 0; x < W; x++) {
        mtrx.dot(x, y, mouth_gfx_on(invert));
    }
    uint32_t dur = ap ? airplay_track_duration_ms() : bt_audio_track_duration_ms();
    uint32_t pos = ap ? airplay_track_position_ms() : bt_audio_track_position_ms();
    int gx = 0;
    if (dur > 1u) {
        if (pos >= dur) {
            pos = dur - 1u;
        }
        gx = (int)((uint64_t)pos * (uint64_t)(W - 1) / (uint64_t)dur);
    }
    if (gx < 0) {
        gx = 0;
    }
    if (gx >= W) {
        gx = W - 1;
    }
    constexpr uint32_t kBtProgBlinkHalfMs = 250u;
    const bool show_pos_marker = ((millis() / kBtProgBlinkHalfMs) & 1u) == 0u;
    if (show_pos_marker) {
        mtrx.dot((uint8_t)gx, (uint8_t)y, mouth_gfx_off(invert));
    }
}

// Фаза хвилі нормована до matrixVizRefreshMs (56 мс). Інакше TTS-кадр крутить лінії швидше.
static float s_analyz_dt_scale = 1.f;

static void analyz_note_frame_dt(bool tts) {
    static uint32_t s_last_ms;
    const uint32_t now = millis();
    uint32_t dt = now - s_last_ms;
    s_last_ms = now;
    const uint32_t ref = tts ? RadioConfig::matrixVizTtsRefreshMs : RadioConfig::matrixVizRefreshMs;
    if (dt < 1u) {
        dt = 1u;
    }
    if (dt > ref * 3u) {
        dt = ref;
    }
    s_analyz_dt_scale = (float)dt / (float)ref;
}

void analyz0(uint8_t vol, bool invert, bool animate = true) {
    static float phi;
    static float phi_chaos;
    static float omega_filt;
    constexpr float two_pi = 6.2831853f;
    const float kdt = s_analyz_dt_scale;

    if (!animate) {
        phi = 0.f;
        phi_chaos = 0.f;
        omega_filt = RadioConfig::analyzSineOmegaMin;
    } else {
        const float omega_tgt = RadioConfig::analyzSineOmegaMin +
                                (float)vol / 100.f * (RadioConfig::analyzSineOmegaMax - RadioConfig::analyzSineOmegaMin);
        const float ease = RadioConfig::analyzSineOmegaEase;
        omega_filt += (omega_tgt - omega_filt) * ease * kdt;
        phi += omega_filt * kdt;
        phi_chaos += omega_filt * RadioConfig::analyzWaveChaosOmegaRatio * kdt;
        while (phi > two_pi * 16.f) {
            phi -= two_pi * 16.f;
        }
        while (phi_chaos > two_pi * 24.f) {
            phi_chaos -= two_pi * 24.f;
        }
    }

    const int W = RadioConfig::analyzWidth;
    const float k = two_pi * RadioConfig::analyzSinePeriodsAcross / (float)W;
    const float k2 = RadioConfig::analyzWaveChaosK2;
    const float fm = animate ? RadioConfig::analyzWaveFmDepth : 0.f;
    const float nmix = animate ? RadioConfig::analyzWaveNoiseMix : 0.f;
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

static void mouth_robot_fill_ctx(uint8_t vol, MouthRobotCtx* c, bool animate) {
    static float phi;
    static float phi2;
    static float phi_slow;
    constexpr float two_pi = 6.2831853f;

    c->v = (float)vol / 100.f;
    const float v = c->v;
    const float kdt = s_analyz_dt_scale;
    if (!animate) {
        phi = 0.f;
        phi2 = 0.f;
        phi_slow = 0.f;
    } else {
        const float omega = RadioConfig::analyzMouthPhiOmegaMin +
                            v * (RadioConfig::analyzMouthPhiOmegaMax - RadioConfig::analyzMouthPhiOmegaMin);
        phi += omega * kdt;
        while (phi > two_pi * 8.f) {
            phi -= two_pi * 8.f;
        }
        const float o2 = RadioConfig::analyzMouthPhi2OmegaMin +
                         v * (RadioConfig::analyzMouthPhi2OmegaMax - RadioConfig::analyzMouthPhi2OmegaMin);
        const float nz_step =
            (float)inoise8((uint8_t)(phi2 * 37.f + phi * 11.f), (uint8_t)(millis() >> 5)) / 255.f;
        const float na = fminf(0.95f, fmaxf(0.f, RadioConfig::analyzMouthOmegaNoiseAmp));
        phi2 += o2 * (1.f - na + na * (0.38f + 0.62f * nz_step)) * kdt;
        while (phi2 > two_pi * 8.f) {
            phi2 -= two_pi * 8.f;
        }
        const float o_s = RadioConfig::analyzMouthSlowOmegaMin +
                          v * (RadioConfig::analyzMouthSlowOmegaMax - RadioConfig::analyzMouthSlowOmegaMin);
        phi_slow += o_s * (0.82f + 0.18f * nz_step) * kdt;
        while (phi_slow > two_pi * 8.f) {
            phi_slow -= two_pi * 8.f;
        }
    }

    c->phi = phi;
    c->phi2 = phi2;
    c->phi_slow = phi_slow;
    c->nz = animate ? ((float)inoise8((uint8_t)(phi2 * 37.f + phi * 11.f), (uint8_t)(millis() >> 5)) / 255.f)
                    : 0.5f;

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
    c->lip_wobble = 0.5f + 0.5f * sinf(phi_slow * 1.47f + phi * 1.9f + c->nz * 4.f);
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

static void mouth_robot_one_frame(uint8_t vol, bool invert, bool animate) {
    MouthRobotCtx ctx;
    mouth_robot_fill_ctx(vol, &ctx, animate);
    int8_t up[32];
    int8_t lo[32];
    mouth_robot_compute_up_lo(&ctx, up, lo);
    mouth_robot_draw_lips(ctx.W, up, lo, invert);
}

}  // namespace

static void analyz_mouth_robot_backup(uint8_t vol, bool invert, bool animate = true) {
    mouth_robot_one_frame(vol, invert, animate);
}

// 0 хвиля; 1 інв.; 2 EQ; 3 рот; 4 рот інв.; 5 прогрес трека (BT / AIR).
static uint8_t mouth_anim_mode() {
    uint8_t m = radioState.mode;
    if (m > 5) {
        m = 0;
        radioState.mode = 0;
    }
    return m;
}

static void draw_mouth_anim(uint8_t v_mouth, bool invert) {
    switch (mouth_anim_mode()) {
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
        case 5:
            analyz_bt_track_progress(invert);
            break;
        default:
            analyz0(v_mouth, false);
            break;
    }
}

// Пауза: той самий режим, перший кадр (фаза 0), без бігу анімації.
static void draw_mouth_anim_rest(bool invert) {
    constexpr uint8_t kRestVis = 42;
    switch (mouth_anim_mode()) {
        case 1:
            analyz0(kRestVis, true, false);
            break;
        case 2:
            analyz_eq_bars(kRestVis, false, true);
            break;
        case 3:
            analyz_mouth_robot_backup(0, false, false);
            break;
        case 4:
            analyz_mouth_robot_backup(0, true, false);
            break;
        case 5:
            analyz_bt_track_progress(invert);
            break;
        default:
            analyz0(kRestVis, false, false);
            break;
    }
}

// Bend the selected resting artwork, rather than replace it with a loading icon.
// Logical coordinates via get()/dot() respect MAX7219 rotation and wiring.
static void draw_ai_thinking_mouth(uint32_t elapsed, uint32_t started_ms) {
    constexpr int W = RadioConfig::analyzWidth;
    static uint8_t resting_columns[W];
    static uint8_t cached_mode = 255;
    static uint32_t cached_start_ms = 0;
    const uint8_t mode = mouth_anim_mode();
    const bool invert = mode == 1 || mode == 4;

    if (cached_mode != mode || cached_start_ms != started_ms) {
        mtrx.rect(0, 0, W - 1, 7, mouth_gfx_off(invert));
        draw_mouth_anim_rest(invert);
        for (int x = 0; x < W; ++x) {
            uint8_t column = 0;
            for (int y = 0; y < 8; ++y) {
                if (mtrx.get(x, y) != invert) {
                    column |= uint8_t(1u << y);
                }
            }
            resting_columns[x] = column;
        }
        cached_mode = mode;
        cached_start_ms = started_ms;
    }

    // Ease into a thoughtful, slightly downturned mouth over 900 ms, then hold.
    // Raising the middle relative to the corners also works for an EQ/progress
    // frame resting on the bottom row, without pushing pixels off the display.
    const float t = elapsed < 900u ? float(elapsed) / 900.f : 1.f;
    const float strength = t * t * (3.f - 2.f * t);
    for (int x = 0; x < W; ++x) {
        const float side = W > 1 ? 2.f * float(x) / float(W - 1) - 1.f : 0.f;
        // Middle +2 px, left corner +1 px, right corner unchanged: an asymmetric
        // arch like the thinking reference, with the right corner sitting lower.
        const float arch = 1.5f * (1.f - side * side) + 0.5f * (1.f - side);
        uint8_t lift = uint8_t(roundf(strength * arch));
        const uint8_t column = resting_columns[x];
        uint8_t room = 0;
        while (room < 8u && !(column & (1u << room))) {
            ++room;
        }
        if (lift > room) {
            lift = room;  // Preserve every foreground pixel, even at the top edge.
        }
        const uint8_t bent = column >> lift;
        for (int y = 0; y < 8; ++y) {
            const bool foreground = (bent & (1u << y)) != 0;
            mtrx.dot(x, y, foreground ? mouth_gfx_on(invert) : mouth_gfx_off(invert));
        }
    }
}

// ========================= SYSTEM =========================
static uint32_t s_wifi_last_activity_ms = 0;

void audio_showstreamtitle(const char* info) {
}

void wifi_touch_activity() {
    s_wifi_last_activity_ms = millis();
}

void syncWifiWithAudioSilence() {
    const wifi_mode_t mode = WiFi.getMode();
    if (mode == WIFI_OFF) {
        return;
    }
    // SoftAP / AP+STA: всегда без modem sleep — инакше WebUI и captive portal падают на части плат.
    if (mode == WIFI_AP || mode == WIFI_AP_STA) {
        WiFi.setSleep(false);
        esp_wifi_set_ps(WIFI_PS_NONE);
        return;
    }

    // STA: при выключенной политике явно снимаем PS (иначе остаётся SDK default MIN_MODEM и даёт сбои).
    if (!RadioConfig::wifiSleepWhenSilent) {
        WiFi.setSleep(false);
        esp_wifi_set_ps(WIFI_PS_NONE);
        return;
    }

    const bool wifiIdleLong =
        (RadioConfig::wifiIdleSleepAfterMs > 0) &&
        ((uint32_t)(millis() - s_wifi_last_activity_ms) >= RadioConfig::wifiIdleSleepAfterMs);
    WiFi.setSleep(wifiIdleLong);
    if (wifiIdleLong) {
        esp_wifi_set_ps(RadioConfig::wifiIdlePsMaxModem ? WIFI_PS_MAX_MODEM : WIFI_PS_MIN_MODEM);
    } else {
        esp_wifi_set_ps(WIFI_PS_NONE);
    }
}

void wifi_request_sta_reconnect(bool softap_on_fail, bool keep_ap) {
    if (strcmp(g_audio_source, "bt") == 0) {
        return;
    }
    if (wifiConnecting) {
        return;
    }
    WifiStored w;
    nvsLoadWifi(w);
    const String staSsid = w.staSsid.length() ? w.staSsid : String(RadioConfig::wifiSsid);
    const String staPass = w.staPass.length() ? w.staPass : String(RadioConfig::wifiPass);

    const wifi_mode_t mode = WiFi.getMode();
    const bool ap_mode = (mode == WIFI_AP || mode == WIFI_AP_STA);
    s_sta_keep_ap = keep_ap && ap_mode;
    if (ap_mode && !s_sta_keep_ap) {
        if (audio.isRunning()) {
            audio.stopSong();
        }
        WiFi.softAPdisconnect(true);
    }

    WiFi.persistent(false);
    WiFi.mode(s_sta_keep_ap ? WIFI_AP_STA : WIFI_STA);
    WiFi.setSleep(false);
    WiFi.setMinSecurity(WIFI_AUTH_WPA_PSK);
    WiFi.disconnect(false, false);
    delay(50);
    Serial.printf("[WiFi] reconnect STA \"%s\" (softap_on_fail=%d)\n", staSsid.c_str(),
                  (int)softap_on_fail);
    WiFi.begin(staSsid.c_str(), staPass.c_str());
    wifiConnecting = true;
    s_sta_connect_started_ms = millis();
    s_sta_softap_on_fail = softap_on_fail;
    s_ap_sta_retry_ms = millis();
    wifi_touch_activity();
    syncWifiWithAudioSilence();
}

void wifi_ap_toggle_from_core0() {
    if (strcmp(g_audio_source, "bt") == 0) {
        return;
    }
    if (wifiConnecting) {
        return;
    }
    WifiStored w;
    nvsLoadWifi(w);
    const String apSsid = nvsEffectiveApSsid(w);
    const String apPwd = nvsEffectiveApPass(w);

    const wifi_mode_t mode = WiFi.getMode();
    const bool ap_mode = (mode == WIFI_AP || mode == WIFI_AP_STA);

    // AP on -> off: к домашней сети; если не вышло — SoftAP снова.
    if (ap_mode) {
        if (WiFi.status() == WL_CONNECTED) {
            WiFi.softAPdisconnect(true); // Disable AP only, keep the live STA connection.
            WiFi.mode(WIFI_STA);
            s_sta_keep_ap = false;
            syncWifiWithAudioSilence();
        } else {
            wifi_request_sta_reconnect(true, false);
        }
        print_val('A', 0);
        return;
    }

    // Keep the upstream connection alive: the portal and AI need AP + STA together.
    const bool staOnline = WiFi.status() == WL_CONNECTED;
    const int channel = staOnline ? WiFi.channel() : 1;
    if (audio.isRunning()) {
        audio.stopSong();
    }
    radioState.state = false;
    WiFi.persistent(false);
    if (!WiFi.mode(WIFI_AP_STA) ||
        !WiFi.softAP(apSsid.c_str(), apPwd.length() >= 8 ? apPwd.c_str() : nullptr, channel)) {
        Serial.println(F("[WiFi] SoftAP start failed"));
        WiFi.mode(WIFI_STA);
        syncWifiWithAudioSilence();
        print_val('A', 0);
        return;
    }
    Serial.printf("[WiFi] AP+STA portal=%s upstream=%s\n", WiFi.softAPIP().toString().c_str(),
                  staOnline ? WiFi.localIP().toString().c_str() : "connecting");
    if (!staOnline) wifi_request_sta_reconnect(false, true);
    wifi_touch_activity();
    syncWifiWithAudioSilence();
    change_state();
    print_val('A', 1);
}

bool wifi_sta_connect(const String& ssid, const String& pass, uint32_t timeout_ms) {
    wifiConnecting = true;
    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    // iPhone hotspot / смешанный WPA2-WPA3: не требовать только WPA3.
    WiFi.setMinSecurity(WIFI_AUTH_WPA_PSK);
    WiFi.disconnect(true, true);
    delay(100);
    Serial.printf("[WiFi] STA connect SSID=\"%s\" …\n", ssid.c_str());
    WiFi.begin(ssid.c_str(), pass.c_str());
    const uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && (uint32_t)(millis() - t0) < timeout_ms) {
        delay(50);
    }
    wifiConnecting = false;
    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("[WiFi] OK %s ch=%d rssi=%d\n", WiFi.localIP().toString().c_str(),
                      (int)WiFi.channel(), (int)WiFi.RSSI());
        syncWifiWithAudioSilence();
        return true;
    }
    const wl_status_t st = WiFi.status();
    Serial.printf("[WiFi] FAIL status=%d (1=idle 4=fail 6=disc). "
                  "ESP32 only 2.4GHz — on iPhone Hotspot enable Maximize Compatibility.\n",
                  (int)st);
    return false;
}

void apply_output_volume() {
    if (bender_ai_owns_speaker() || airplay_owns_speaker()) {
        return;
    }
    int8_t vol = radioState.vol;
    if (vol > RadioConfig::ampVolumeUiMax) {
        vol = RadioConfig::ampVolumeUiMax;
    }
    if (vol < 0) {
        vol = 0;
    }
    if (strcmp(g_audio_source, "bt") == 0) {
        bt_audio_volume_apply(radioState.state, vol);
    } else {
        audio.setVolume(radioState.state ? vol : 0);
    }
    if (RadioConfig::ampMutePin == 255) {
        return;
    }
    pinMode(RadioConfig::ampMutePin, OUTPUT);
    if (!RadioConfig::ampUseHardwareMute) {
        // MOSFET выкл: SD только выбирает L/R по резисторам схемы.
        digitalWrite(RadioConfig::ampMutePin, RadioConfig::ampMuteWhenIdleHigh ? LOW : HIGH);
        return;
    }
    const bool silent = !radioState.state || vol <= 0;
    const bool mute_high = RadioConfig::ampMuteWhenIdleHigh ? silent : !silent;
    digitalWrite(RadioConfig::ampMutePin, mute_high ? HIGH : LOW);
}

void free_uart0_from_i2s_pins() {
#if defined(CONFIG_IDF_TARGET_ESP32S3)
    // DevKit: UART0 RX=GPIO44, TX=GPIO43. Наш I2S BCLK = 44.
    // При USB без CDC Serial садится на UART0 → оба усилителя молчат.
    Serial0.end();
#endif
}

void amp_force_mute() {
    if (RadioConfig::ampMutePin == 255) {
        return;
    }
    pinMode(RadioConfig::ampMutePin, OUTPUT);
    if (!RadioConfig::ampUseHardwareMute) {
        digitalWrite(RadioConfig::ampMutePin, RadioConfig::ampMuteWhenIdleHigh ? LOW : HIGH);
        return;
    }
    digitalWrite(RadioConfig::ampMutePin, RadioConfig::ampMuteWhenIdleHigh ? HIGH : LOW);
}

void audio_hw_init(bool log_serial) {
    static bool s_bufsize_done = false;
    if (!s_bufsize_done) {
        audio.setBufsize(RadioConfig::radioBuffer, RadioConfig::radioBufferPsram);
        s_bufsize_done = true;
    }
    amp_force_mute();
    free_uart0_from_i2s_pins();
    (void)audio.reclaimI2SChannel();
    if (RadioConfig::pcm5102XsmtPin != 255) {
        pinMode(RadioConfig::pcm5102XsmtPin, OUTPUT);
        digitalWrite(RadioConfig::pcm5102XsmtPin, RadioConfig::pcm5102XsmtActiveHigh ? HIGH : LOW);
    }
    const bool pins_ok =
        audio.setPinout(RadioConfig::i2sBclk, RadioConfig::i2sLrc, RadioConfig::i2sDout);
    audio.forceMono(RadioConfig::ampForceMono);
    // Как yoRadio player.init: баланс/тон в ноль (иначе мусор после прошлых экспериментов).
    audio.setBalance(0);
    audio.setTone(0, 0, 0);
    audio.setVolume(0);
    audio.setConnectionTimeout(1200, 2000);
    if (log_serial) {
        Serial.printf("I2S BCK=%u LRCK=%u DIN=%u mute=%u set_pin=%s vol=%d state=%d\n",
                      (unsigned)RadioConfig::i2sBclk, (unsigned)RadioConfig::i2sLrc,
                      (unsigned)RadioConfig::i2sDout, (unsigned)RadioConfig::ampMutePin,
                      pins_ok ? "OK" : "FAIL", (int)radioState.vol, (int)radioState.state);
    }
}

// Pololu OFF: короткий HIGH → VOUT гаснет (банка больше не кормит понижайку).
// На OFF нужно >1 V; GPIO даёт 3.3 V. Если руками выключало только от VIN (8 V) —
// поставь NPN/PNP: GPIO7 открывает путь VIN→OFF.
static void pololu_power_cut() {
    if (RadioConfig::pololuOffPin == 255) {
        return;
    }
    pinMode(RadioConfig::pololuOffPin, OUTPUT);
    digitalWrite(RadioConfig::pololuOffPin, LOW);
    delay(5);
    // Два импульса — надёжнее одного короткого.
    for (uint8_t i = 0; i < 2; i++) {
        digitalWrite(RadioConfig::pololuOffPin, HIGH);
        delay(RadioConfig::pololuOffPulseMs);
        digitalWrite(RadioConfig::pololuOffPin, LOW);
        delay(30);
    }
    Serial.printf("[Power] Pololu OFF pulse GPIO%u %ums x2\n", (unsigned)RadioConfig::pololuOffPin,
                  (unsigned)RadioConfig::pololuOffPulseMs);
}

// Критический заряд: уводим в deep sleep без wake sources — меньше ток, чем у «живой» прошивки
// (типичный цикл: Brownout → reset → снова нагрузка → снова Brownout).
static void low_battery_enter_deep_sleep_forever() {
    airplay_interrupt();
    amp_force_mute();
    radioState.state = false;
    if (strcmp(g_audio_source, "wifi") == 0) {
        audio.setVolume(0);
        if (audio.isRunning()) {
            audio.stopSong();
        }
        WiFi.disconnect(true);
        WiFi.mode(WIFI_OFF);
    } else {
        bt_audio_volume_apply(false, 0);
        bt_audio_stop_sink();
        WiFi.mode(WIFI_OFF);
    }
    delay(120);
    uint8_t br_off[] = {0, 0, 0, 0, 0};
    mtrx.setBright(br_off);
    mtrx.clear();
    matrix_flush();
    (void)esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_EXT1);
    (void)esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);
    (void)esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TOUCHPAD);
#if defined(ESP_SLEEP_WAKEUP_ULP)
    (void)esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ULP);
#endif
    // Сначала рвём Pololu (если пропаян). Если питание ушло — сюда уже не вернёмся.
    pololu_power_cut();
    // Fallback: deep sleep + кнопка будить (если OFF не сработал / пин не подключён).
    esp_sleep_enable_ext0_wakeup((gpio_num_t)RadioConfig::encBtn, 0);
    esp_deep_sleep_start();
    delay(1000);
}

static void battery_shutdown_guard_on_sample() {
    const auto reason = battery_shutdown_reason();
    if (reason == BatteryShutdownReason::None) return;
    Serial.printf("[Batt] %s SLEEP raw_mv=%u display_mv=%u pct=%u chg=%d\n",
                  reason == BatteryShutdownReason::Critical ? "CRITICAL" : "LOW",
                  (unsigned)battery_protection_millivolts(), (unsigned)battery_millivolts(),
                  (unsigned)battery_percent(), (int)battery_is_charging());
    low_battery_enter_deep_sleep_forever();
}

static void radio_enter_deep_sleep() {
    airplay_interrupt();
    memory.update();
    if (radioState.state) {
        if (strcmp(g_audio_source, "bt") == 0) {
            bt_audio_volume_apply(false, 0);
        } else {
            audio.setVolume(0);
            audio.stopSong();
        }
    }
    {
        uint8_t br_off[] = {0, 0, 0, 0, 0};
        mtrx.setBright(br_off);
        mtrx.clear();
        matrix_flush();
    }
    Serial.println(F("[Power] sleep → Pololu OFF then deep sleep"));
    delay(50);
    pololu_power_cut();
    delay(100);
    // Если Pololu срезал питание — сюда не дойдём. Иначе fallback deep sleep.
    esp_sleep_enable_ext0_wakeup((gpio_num_t)RadioConfig::encBtn, 0);
    esp_deep_sleep_start();
}

void core0(void* p) {
    // ========================= SETUP =========================
    if (RadioConfig::pololuOffPin != 255) {
        pinMode(RadioConfig::pololuOffPin, OUTPUT);
        digitalWrite(RadioConfig::pololuOffPin, LOW);  // не держать OFF при загрузке
    }
    EncButton eb(RadioConfig::encS1, RadioConfig::encS2, RadioConfig::encBtn);
    // Было 480 — при SoftAP/без сети тики реже, серии 6–9 кликов рвались.
    eb.setClickTimeout(900);
    eb.setDebTimeout(60);
    eb.setEncType(EB_STEP4_LOW);
    Tmr viz_tmr(RadioConfig::matrixVizRefreshMs);
    Tmr tts_mouth_tmr(RadioConfig::matrixVizTtsRefreshMs);
    Tmr eye_tmr(RadioConfig::matrixEyeRefreshMs);
    Tmr matrix_tmr(1000);
    Tmr angry_tmr(RadioConfig::mpu6050AngryEyesMs);
    Tmr pong_tmr(145);
    matrix_tmr.timerMode(1);
    angry_tmr.timerMode(1);
    bool pulse = 0;
    uint8_t pcm_pulse_l = 0;
    static uint32_t enc_btn_press_ms = 0;
    // Удерж.+поворот (станция/яркость/громкость): не трактовать как длинное удержание → сон / restart.
    static bool s_enc_hold_had_turn_while_pressed = false;
    // BT: 4 клика + удержание без поворота — сброс сопряжений и вход в поиск нового телефона.
    static bool s_bt_forget_pair_hold_ready = false;
    static bool s_ptt_this_press = false;
    BenderFaceState previous_ai_face = BenderFaceState::Idle;
    uint32_t ai_face_since_ms = 0;
    uint32_t ai_face_frame_ms = 0;

    EEPROM.begin(memory.blockSize());
    memory.begin(0, 'b');
    // Не поднимать стрим на холодном старте из EEPROM — connecttohost + I2S + усилители → brownout.
    radioState.state = false;
    if (radioState.vol > RadioConfig::ampVolumeUiMax) {
        radioState.vol = RadioConfig::ampVolumeUiMax;
    }
    apply_output_volume();
    {
        uint8_t b = 0;
        if (nvsTakePendingBrightnessOverride(b)) {
            radioState.bright_eyes = (int8_t)b;
            radioState.bright_mouth = (int8_t)b;
            memory.update();
        }
    }
    nvsLoadCustomStations(s_custom_stations, RadioConfig::customStationMaxCount, s_custom_station_count);
    nvsLoadMatrixBrightnessTrim(s_matrix_brightness_trim, RadioConfig::matrixModuleCount);
    s_wake_on_shake = nvsLoadWakeOnShake();
    s_face_calm_after_ms = uint32_t(nvsLoadCalmMinutes()) * 60000u;
    Serial.printf("[Motion] calm after %lu min (0=never)\n", (unsigned long)(s_face_calm_after_ms / 60000u));
    Serial.printf("[Motion] wake on shake %s\n", s_wake_on_shake ? "ON" : "OFF");
    Serial.printf("[Radio] stations: %u built-in + %u NVS = %u total\n",
                  (unsigned)kStationBuiltInCount, (unsigned)s_custom_station_count,
                  (unsigned)station_total_count());
    for (uint8_t i = 0; i < kStationBuiltInCount; i++) {
        Serial.printf("[Radio]  %u %s\n", (unsigned)i, stations[i]);
    }

    {
        uint32_t matrixPreDelay = RadioConfig::matrixPowerStabilizeBeforeBeginMs;
        if (RadioConfig::matrixPowerStabilizeBeforeBeginMsAfterWakeMs > 0 &&
            esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT0) {
            matrixPreDelay = RadioConfig::matrixPowerStabilizeBeforeBeginMsAfterWakeMs;
        } else if (g_warm_boot_after_mode_switch &&
                   RadioConfig::matrixPowerStabilizeBeforeBeginMsAfterWakeMs > 0) {
            matrixPreDelay = RadioConfig::matrixPowerStabilizeBeforeBeginMsAfterWakeMs;
        }
        delay(matrixPreDelay);
    }
    // Глобальный MAX7219 в GyverMAX7219 уже вызывает begin() в конструкторе до стабилизации питания —
    // на холодном старте переинициализируем и «промываем» цепочку.
    const bool matrixColdPowerOn =
        (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_UNDEFINED) && !g_warm_boot_after_mode_switch;
    mtrx.begin();
    if (matrixColdPowerOn && RadioConfig::matrixColdBootSecondBeginDelayMs > 0) {
        delay(RadioConfig::matrixColdBootSecondBeginDelayMs);
        mtrx.begin();
    }
    mtrx.setBright((uint8_t)0);
    if (matrixColdPowerOn && RadioConfig::matrixColdBootFlushCycles > 0) {
        for (uint8_t i = 0; i < RadioConfig::matrixColdBootFlushCycles; i++) {
            mtrx.clearDisplay();
            mtrx.clear();
            matrix_flush(true);
            if (RadioConfig::matrixColdBootFlushGapMs > 0) {
                delay(RadioConfig::matrixColdBootFlushGapMs);
            }
        }
    } else {
        mtrx.clear();
        matrix_flush(true);
    }
    if (!g_warm_boot_after_mode_switch) {
        delay(RadioConfig::coldStartMatrixZeroMs);
        delay(RadioConfig::coldStartAfterMatrixMs);
    }

    {
        uint32_t addMs = RadioConfig::matrixDisplayEnableDelayMs;
        if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT0) {
            addMs = RadioConfig::matrixDisplayEnableDelayMsAfterWakeMs;
        } else if (g_warm_boot_after_mode_switch) {
            addMs = RadioConfig::matrixDisplayEnableDelayMsAfterWakeMs;
        }
        g_matrix_display_enable_ms = millis() + addMs;
    }
    upd_bright();
    mtrx.clear();
    // Сразу глаза — чтобы видно, что матрица жива ещё до Wi‑Fi.
    draw_eyes_radio_idle_off();
    matrix_flush(true);
    s_matrix_ui_started = true;

    audio_hw_init(true);
    apply_output_volume();
    station_clamp_index();
    // Стрим только после клика play (radioState.state) — иначе ребут-цикл при Wi‑Fi+I2S на старте.
    reconnect = nullptr;

    battery_init();
    // Первый замер делаем сразу на старте, а не через интервальный таймер.
    battery_force_sample();
    battery_shutdown_guard_on_sample();
    mpu6050_init();

    s_wifi_last_activity_ms = millis();
    if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT0 && RadioConfig::wakeAfterSleepAnimMs > 0) {
        s_wake_after_sleep_anim_until_ms = millis() + RadioConfig::wakeAfterSleepAnimMs;
        s_pending_change_state_after_wake = true;
    }
    syncWifiWithAudioSilence();

    Serial.printf("Matrix CLK=%u CS=%u DAT=%u, base bright=%u\n", RadioConfig::mtrxClk,
                  RadioConfig::mtrxCs, RadioConfig::mtrxDat, matrix_get_base_brightness());

    // ========================= LOOP =========================
    for (;;) {
        if (wifiConnecting && s_sta_connect_started_ms) {
            if (WiFi.status() == WL_CONNECTED) {
                wifiConnecting = false;
                s_sta_connect_started_ms = 0;
                Serial.printf("[WiFi] OK %s\n", WiFi.localIP().toString().c_str());
                if (radioState.state && strcmp(g_audio_source, "wifi") == 0) {
                    reconnect = station_url_for_current();
                }
                syncWifiWithAudioSilence();
                change_state();
            } else if ((uint32_t)(millis() - s_sta_connect_started_ms) > 20000u) {
                wifiConnecting = false;
                s_sta_connect_started_ms = 0;
                if (s_sta_keep_ap) {
                    // Retain the portal while the router is absent. Retry STA later.
                    Serial.println(F("[WiFi] STA timeout — portal stays up, retry later"));
                } else if (s_sta_softap_on_fail) {
                    Serial.println(F("[WiFi] STA timeout — SoftAP back"));
                    WifiStored wfail;
                    nvsLoadWifi(wfail);
                    const String apSsidFail = nvsEffectiveApSsid(wfail);
                    const String apPwdFail = nvsEffectiveApPass(wfail);
                    WiFi.mode(WIFI_AP_STA);
                    s_ap_sta_retry_ms = millis();
                    if (apPwdFail.length() >= 8) {
                        WiFi.softAP(apSsidFail.c_str(), apPwdFail.c_str());
                    } else {
                        WiFi.softAP(apSsidFail.c_str());
                    }
                    print_val('A', 1);
                } else {
                    Serial.println(F("[WiFi] STA timeout — click play to retry"));
                }
                radioState.state = false;
                apply_output_volume();
                syncWifiWithAudioSilence();
                change_state();
            }
        }
        if (WiFi.getMode() == WIFI_AP_STA && !wifiConnecting && WiFi.status() != WL_CONNECTED &&
            strcmp(g_audio_source, "wifi") == 0 && uint32_t(millis() - s_ap_sta_retry_ms) >= 30000u) {
            wifi_request_sta_reconnect(false, true);
        }
        if (s_matrix_brightness_trim_dirty && matrix_display_ready()) {
            s_matrix_brightness_trim_dirty = false;
            upd_bright();
        }
        battery_update();
        // Also consumes a shutdown decision from a manual gauge sample.
        battery_shutdown_guard_on_sample();
        matrix_tmr.tick();
        static bool charge_was_detected = false;
        const bool charge_detected = battery_is_charging();
        if (charge_detected && !charge_was_detected && !pong_active() && !s_mode_pick_active) {
            s_batt_matrix_overlay = true;
            s_batt_matrix_overlay_pct = battery_percent();
            s_batt_charge_frame = 0;
            s_batt_icon_step_ms = millis();
            s_batt_overlay_prev_chg = true;
            print_batt_overlay(s_batt_matrix_overlay_pct);
            matrix_tmr.start(RadioConfig::batteryPercentShowDurationChargingMs);
        }
        charge_was_detected = charge_detected;
        const int requested_volume = s_ui_vol_pending.exchange(-1);
        if (requested_volume >= 0) {
            s_batt_matrix_overlay = false;
            print_val('v', (uint8_t)requested_volume);
            matrix_tmr.start(RadioConfig::matrixOverlayDigitsMs);
        }
        if (s_mode_flash_req) {
            s_mode_flash_req = false;
            s_batt_matrix_overlay = false;
            draw_current_play_mode_mouth();
            matrix_flush();
            matrix_tmr.start(RadioConfig::matrixOverlayDigitsMs);
        }
        if (s_batt_matrix_overlay && !matrix_tmr.state()) {
            s_batt_matrix_overlay = false;
            s_batt_overlay_prev_chg = false;
        }
        if (matrix_display_ready() && s_batt_matrix_overlay && matrix_tmr.state()) {
            const bool chg = RadioConfig::chargingDetectEnable && battery_is_charging();
            const uint8_t pct = battery_percent();
            const bool pct_changed = pct != s_batt_matrix_overlay_pct;
            s_batt_matrix_overlay_pct = pct;
            if (pct_changed || chg != s_batt_overlay_prev_chg)
                print_batt_overlay(s_batt_matrix_overlay_pct);
            if (chg) {
                const uint32_t now = millis();
                if ((uint32_t)(now - s_batt_icon_step_ms) >= RadioConfig::batteryChargeIconAnimStepMs) {
                    s_batt_icon_step_ms = now;
                    s_batt_charge_frame++;
                    print_batt_overlay(s_batt_matrix_overlay_pct);
                }
            }
            s_batt_overlay_prev_chg = chg;
        }
        angry_tmr.tick();
        memory.tick();

        if (mpu6050_poll_shake()) {
            const bool awake = matrix_face_awake();
            if (bender_event_allows_motion_wake(BenderEvent::Shake, awake, s_wake_on_shake)) {
                // Wake the idle face explicitly, independently of voice/event availability.
                s_face_last_live_ms = millis();
                wifi_touch_activity();
                angry_tmr.start();
                Serial.println(awake ? F("[MPU] → angry eyes (shake OK)") : F("[Motion] shake → awake"));
                react_to_life(BenderEvent::Shake);
            }
        }
        react_to_life(mpu6050_motion_event());
        react_to_life(s_life.update(millis(), battery_gauge_ready() && battery_sense_present(),
                                   battery_percent(), battery_is_charging(), WiFi.status() == WL_CONNECTED,
                                   strcmp(g_audio_source, "wifi") == 0 && radioState.state,
                                   radioState.state && strcmp(g_audio_source, "wifi") == 0,
                                   (int)radioState.station, bender_ai_favorite_station((int)radioState.station)));

        if (s_pending_change_state_after_wake) {
            if ((int32_t)(millis() - s_wake_after_sleep_anim_until_ms) >= 0) {
                if (matrix_display_ready()) {
                    s_pending_change_state_after_wake = false;
                    s_wake_after_sleep_anim_until_ms = 0;
                    change_state();
                    s_matrix_ui_started = true;
                }
            }
        }
        const bool show_wake_after_sleep_anim =
            s_pending_change_state_after_wake &&
            (int32_t)(millis() - s_wake_after_sleep_anim_until_ms) < 0;

        if (matrix_display_ready() && !s_matrix_ui_started) {
            s_matrix_ui_started = true;
            change_state();
        }

        if (!matrix_display_ready()) {
            static uint32_t s_matrix_hold_dark_ms;
            if ((uint32_t)(millis() - s_matrix_hold_dark_ms) >= 400) {
                s_matrix_hold_dark_ms = millis();
                uint8_t br0[5] = {0, 0, 0, 0, 0};
                mtrx.setBright(br0);
                mtrx.clear();
                matrix_flush();
            }
        }

        const bool eb_tick = eb.tick();
        if (radioState.state || bender_ai_busy() || airplay_playing()) {
            s_wifi_last_activity_ms = millis();
        } else if (eb_tick && (eb.press() || eb.release() || eb.turn())) {
            s_wifi_last_activity_ms = millis();
        }
        if (eb_tick && eb.press()) {
            enc_btn_press_ms = millis();
            s_enc_hold_had_turn_while_pressed = false;
            s_bt_forget_pair_hold_ready = false;
            s_ptt_this_press = false;
            // Без STA не будить AI: иначе каждый клик → WSS connect fail и клики «сыпаются».
            const wifi_mode_t wm = WiFi.getMode();
            const bool softap_only =
                (wm == WIFI_AP) || (wm == WIFI_AP_STA && WiFi.status() != WL_CONNECTED);
            if (!pong_active() && !s_mode_pick_active && !show_wake_after_sleep_anim &&
                strcmp(g_audio_source, "bt") != 0 && !softap_only &&
                WiFi.status() == WL_CONNECTED) {
                bender_ai_ptt_arm();
            }
        }
        if (eb_tick && eb.release()) {
            if (s_ptt_this_press) {
                bender_ai_ptt_up();
                s_ptt_this_press = false;
            } else {
                bender_ai_ptt_cancel();
            }
        }
        if (eb_tick && eb.pressing() && !s_ptt_this_press && !s_enc_hold_had_turn_while_pressed &&
            eb.getClicks() == 0 && !pong_active() && !s_mode_pick_active &&
            !show_wake_after_sleep_anim && strcmp(g_audio_source, "bt") != 0 &&
            eb.pressFor() >= RadioConfig::encoderPttHoldMs && !bender_ai_recording()) {
            s_ptt_this_press = true;
            bender_ai_ptt_down();
        }
        // Энкодер не глушим на время STA-подключения — иначе жест «4 клика + поворот» не работает до ~25 с.
        const bool eb_e = (!show_wake_after_sleep_anim && eb_tick);
        if (eb_tick && eb.pressing() && !s_enc_hold_had_turn_while_pressed &&
            strcmp(g_audio_source, "bt") == 0 && eb.getClicks() == 3 &&
            eb.pressFor() >= RadioConfig::btForgetPairedHoldMs) {
            s_bt_forget_pair_hold_ready = true;
        }

        if (matrix_display_ready()) {
            if (strcmp(g_audio_source, "bt") == 0) {
                const uint8_t r = bt_audio_take_remote_ui_request();
                if (r == 1u && radioState.state) {
                    radioState.state = false;
                    apply_output_volume();
                    syncWifiWithAudioSilence();
                    change_state();
                } else if (r == 2u && !radioState.state) {
                    radioState.state = true;
                    apply_output_volume();
                    syncWifiWithAudioSilence();
                    change_state();
                }
            }
        // Только core0 трогает MAX7219: вызов anim_search с core1 в setup() давал гонку и мигание при Wi‑Fi.
        if (show_wake_after_sleep_anim) {
            anim_search();
        } else if (pong_active()) {
            if (pong_tmr.tick()) {
                pong_step();
                (void)pong_goal_fx_active();
                pong_draw();
                draw_eyes_follow_ball(pong_ball_x(), pong_ball_y());
                pong_sync_matrix_brightness();
                matrix_flush();
            }
            if (eb_e) {
                if (eb.turn()) {
                    if (eb.pressing()) {
                        s_enc_hold_had_turn_while_pressed = true;
                        s_bt_forget_pair_hold_ready = false;
                        bender_ai_ptt_cancel();
                    } else {
                        pong_paddle_nudge(eb.dir());
                        pong_draw();
                        draw_eyes_follow_ball(pong_ball_x(), pong_ball_y());
                        pong_sync_matrix_brightness();
                        matrix_flush();
                    }
                }
                if (eb.hasClicks()) {
                    const uint8_t n = eb.getClicks();
                    if (n == 1 && pong_serve_waiting()) {
                        pong_resume_after_goal();
                        pong_draw();
                        draw_eyes_follow_ball(pong_ball_x(), pong_ball_y());
                        pong_sync_matrix_brightness();
                        matrix_flush();
                    } else if (n == RadioConfig::encoderPongClicks) {
                        pong_set_active(false);
                        upd_bright();
                        matrix_flush();
                    } else if (n == RadioConfig::encoderSoftApToggleClicks) {
                        if (strcmp(g_audio_source, "wifi") == 0) {
                            wifi_ap_toggle_from_core0();
                        }
                    } else if (n == RadioConfig::encoderSleepClicks) {
                        radio_enter_deep_sleep();
                    } else if (n == RadioConfig::encoderRestartClicks) {
                        memory.update();
                        ESP.restart();
                    }
                }
                memory.update();
            }
        } else {
            BenderFaceState ai_face = bender_ai_face_state();
            const uint32_t face_now = millis();
            if (ai_face == BenderFaceState::Idle && bender_face_recent(face_now, s_life_face_since, 2200)) {
                ai_face = bender_event_face(s_life_face);
            }
            const bool ai_face_changed = ai_face != previous_ai_face;
            if (ai_face_changed) {
                previous_ai_face = ai_face;
                ai_face_since_ms = face_now;
                Serial.printf("[Face] %s\n", ai_face == BenderFaceState::Listening ? "listening" :
                              ai_face == BenderFaceState::Thinking ? "thinking" :
                              ai_face == BenderFaceState::Speaking ? "speaking" :
                              ai_face == BenderFaceState::Error ? "error" :
                              ai_face == BenderFaceState::Annoyed ? "annoyed" :
                              ai_face == BenderFaceState::Pleased ? "pleased" :
                              ai_face == BenderFaceState::Curious ? "curious" :
                              ai_face == BenderFaceState::Tired ? "tired" : "idle");
            }
            if (ai_face != BenderFaceState::Idle && !s_mode_pick_active) {
                s_face_last_live_ms = face_now;
                const uint32_t frame_ms = ai_face == BenderFaceState::Speaking
                                              ? RadioConfig::matrixVizTtsRefreshMs : 50u;
                if (ai_face_changed || uint32_t(face_now - ai_face_frame_ms) >= frame_ms) {
                    ai_face_frame_ms = face_now;
                    upd_bright();
                    draw_ai_eyes(ai_face, uint32_t(face_now - ai_face_since_ms));
                    if (!matrix_tmr.state()) {
                        if (ai_face == BenderFaceState::Speaking) {
                            const uint8_t mode = mouth_anim_mode();
                            const bool invert = mode == 1 || mode == 4;
                            mtrx.rect(0, 0, RadioConfig::analyzWidth - 1, 7, mouth_gfx_off(invert));
                            analyz_note_frame_dt(true);
                            const uint8_t level = pcm_wave_level_after_gate();
                            // Track-progress mode has no meaning for a spoken reply.
                            if (mode == 5) {
                                analyz_mouth_robot_backup(level, false);
                            } else if (level > 0) {
                                draw_mouth_anim(level, invert);
                            } else {
                                draw_mouth_anim_rest(invert);
                            }
                        } else if (ai_face == BenderFaceState::Listening || ai_face == BenderFaceState::Pleased ||
                                   ai_face == BenderFaceState::Curious || ai_face == BenderFaceState::Tired ||
                                   ai_face == BenderFaceState::Annoyed) {
                            const uint8_t mode = mouth_anim_mode();
                            const bool invert = mode == 1 || mode == 4;
                            mtrx.rect(0, 0, RadioConfig::analyzWidth - 1, 7, mouth_gfx_off(invert));
                            draw_mouth_anim_rest(invert);
                        } else if (ai_face == BenderFaceState::Thinking) {
                            draw_ai_thinking_mouth(uint32_t(face_now - ai_face_since_ms), ai_face_since_ms);
                        } else {
                            draw_ai_error_mouth();
                        }
                    }
                    matrix_flush();
                }
            } else if ((wifiConnecting || (strcmp(g_audio_source, "bt") == 0 && bt_audio_needs_pairing_ui())) &&
                !s_mode_pick_active) {
                anim_search();
            } else {
            if (matrix_face_awake()) {
                if (ai_face_changed || eye_tmr) {
                    upd_bright();
                    if (battery_sad_eyes_wanted()) {
                        draw_battery_sad_eyes_both();
                        matrix_flush();
                    } else {
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
                        matrix_flush();
                    }
                }
            } else {
                if (ai_face_changed || eye_tmr) {
                    upd_bright();
                    // Радио выкл.: рот не визуализируется — после оверлея батареи (matrix_tmr) цифры иначе не снимаются.
                    if (!matrix_tmr.state()) {
                        mtrx.rect(0, 0, RadioConfig::analyzWidth - 1, 7, GFX_CLEAR);
                    }
                    if (battery_sad_eyes_wanted()) {
                        draw_battery_sad_eyes_both();
                    } else {
                        draw_eyes_radio_idle_off();
                    }
                    matrix_flush();
                }
            }

            // Режимы рта 0…5: хвиля / інв. / EQ / рот / рот інв. / прогрес трека (BT / AIR).
            (void)mouth_anim_mode();
            if (s_mode_pick_active) {
                upd_bright();
                draw_mode_pick_mouth();
                matrix_flush();
            } else if ((ai_face_changed || viz_tmr || (bender_ai_tts_playing() && tts_mouth_tmr)) && !matrix_tmr.state() &&
                       radioState.mode <= 5 && matrix_face_awake()) {
                const bool talk = bender_ai_tts_playing() || airplay_playing() ||
                                  (radioState.state && !bender_ai_busy());
                const uint8_t anim_mode = mouth_anim_mode();
                const bool mouth_invert = (anim_mode == 1 || anim_mode == 4);
                mtrx.rect(0, 0, RadioConfig::analyzWidth - 1, 7, mouth_gfx_off(mouth_invert));
                analyz_note_frame_dt(bender_ai_tts_playing());
                if (talk) {
                const bool bender_talk = bender_ai_tts_playing();
                const uint8_t vol =
                    bender_talk ? (uint8_t)g_pcm_vis : pcm_vis_after_noise_gate(g_pcm_vis);
                if (vol > pcm_pulse_l + 12) {
                    pulse = 1;
                }
                pcm_pulse_l = (uint8_t)((pcm_pulse_l * 3u + vol) / 4u);

                uint8_t v_mouth = pcm_wave_level_after_gate();
                static bool s_bender_live = false;
                static uint32_t s_bender_quiet_ms = 0;
                if (bender_talk) {
                    s_bender_live = true;
                    s_bender_quiet_ms = 0;
                    draw_mouth_anim(v_mouth > 0 ? v_mouth : (uint8_t)22, mouth_invert);
                } else if (airplay_playing()) {
                    s_bender_live = false;
                    s_bender_quiet_ms = 0;
                    draw_mouth_anim(v_mouth, mouth_invert);
                } else {
                    s_bender_live = false;
                    s_bender_quiet_ms = 0;
                    if (v_mouth < 22) {
                        v_mouth = 22;
                    }
                    draw_mouth_anim(v_mouth, mouth_invert);
                }
                } else {
                    pcm_pulse_l = 0;
                    draw_mouth_anim_rest(mouth_invert);
                }
                matrix_flush();
            }

            }

            if (eb_e) {
                static bool station_changed = 0;

                // hasClicks() до turn(): иначе на том же тике поворот уходит в громкость.
                if (eb.hasClicks() && !s_ptt_this_press) {
                    const uint8_t clicks = eb.getClicks();
                    if (play_mode_is_airplay() && clicks >= 1 && clicks <= 3) {
                        if (clicks == 1) {
                            airplay_dacp_command("playpause");
                        } else if (clicks == 2) {
                            airplay_dacp_command("nextitem");
                        } else {
                            airplay_dacp_command("previtem");
                        }
                    } else {
                    switch (clicks) {
                        case 1:
                            radioState.state = !radioState.state;
                            if (radioState.state) {
                                bender_ai_yield_radio();
                            }
                            if (!radioState.state) {
                                if (strcmp(g_audio_source, "bt") == 0) {
                                    bt_audio_avrcp_pause();
                                } else {
                                    audio.setVolume(0);
                                    audio.stopSong();
                                }
                            } else if (strcmp(g_audio_source, "wifi") == 0) {
                                if (WiFi.status() == WL_CONNECTED) {
                                    reconnect = station_url_for_current();
                                } else if (wifiConnecting) {
                                    Serial.println(F("[Radio] wait WiFi…"));
                                } else {
                                    // Офлайн / SoftAP: play = переподключить STA, потом станцию.
                                    Serial.println(F("[Radio] no WiFi → reconnect then play"));
                                    wifi_request_sta_reconnect(false);
                                }
                            } else if (strcmp(g_audio_source, "bt") == 0) {
                                bt_audio_avrcp_play();
                            }
                            apply_output_volume();
                            syncWifiWithAudioSilence();
                            change_state();
                            break;
                        case 2:
                            if (strcmp(g_audio_source, "wifi") == 0) {
                                radio_step_station(+1);
                                print_val('s', radioState.station);
                                Serial.printf("[Radio] next station %d/%u %s\n",
                                              (int)radioState.station, (unsigned)station_total_count(),
                                              station_url_by_index(radioState.station));
                                s_batt_matrix_overlay = false;
                                matrix_tmr.start(RadioConfig::matrixOverlayDigitsMs);
                                if (WiFi.status() == WL_CONNECTED) {
                                    reconnect = station_url_for_current();
                                } else if (radioState.state && !wifiConnecting) {
                                    wifi_request_sta_reconnect(false);
                                }
                            }
                            break;
                        case 3:
                            if (strcmp(g_audio_source, "wifi") == 0) {
                                radio_step_station(-1);
                                print_val('s', radioState.station);
                                Serial.printf("[Radio] prev station %d/%u %s\n",
                                              (int)radioState.station, (unsigned)station_total_count(),
                                              station_url_by_index(radioState.station));
                                s_batt_matrix_overlay = false;
                                matrix_tmr.start(RadioConfig::matrixOverlayDigitsMs);
                                if (WiFi.status() == WL_CONNECTED) {
                                    reconnect = station_url_for_current();
                                } else if (radioState.state && !wifiConnecting) {
                                    wifi_request_sta_reconnect(false);
                                }
                            } else {
                                radioState.trsh = (uint16_t)constrain((int)g_pcm_level_adc * 2 / 3, 4, 3800);
                            }
                            break;
                        case 4:
                            if (RadioConfig::airplayEnable) {
                                commitPlayModeSwitch(play_mode_is_airplay() ? "radio" : "ap");
                            }
                            break;
                        case RadioConfig::encoderBatteryClicks:
                            if (RadioConfig::batteryMonitorEnable) {
                                battery_force_sample();
                                s_batt_matrix_overlay = true;
                                s_batt_matrix_overlay_pct = battery_percent();
                                s_batt_charge_frame = 0u;
                                s_batt_icon_step_ms = millis();
                                s_batt_overlay_prev_chg = battery_is_charging();
                                print_batt_overlay(s_batt_matrix_overlay_pct);
                                {
                                    const uint16_t show_ms =
                                        s_batt_overlay_prev_chg
                                            ? (uint16_t)RadioConfig::batteryPercentShowDurationChargingMs
                                            : (uint16_t)RadioConfig::batteryPercentShowDurationMs;
                                    matrix_tmr.start(show_ms);
                                }
                            }
                            break;
                        case RadioConfig::encoderSoftApToggleClicks:
                            if (strcmp(g_audio_source, "wifi") == 0) {
                                wifi_ap_toggle_from_core0();
                                s_batt_matrix_overlay = false;
                                matrix_tmr.start(RadioConfig::matrixOverlayDigitsMs);
                            }
                            break;
                        case RadioConfig::encoderPongClicks:
                            pong_start();
                            pong_tmr.start();
                            pong_draw();
                            draw_eyes_follow_ball(pong_ball_x(), pong_ball_y());
                            pong_sync_matrix_brightness();
                            matrix_flush();
                            break;
                        case RadioConfig::encoderSleepClicks:
                            radio_enter_deep_sleep();
                            break;
                        case RadioConfig::encoderRestartClicks:
                            memory.update();
                            ESP.restart();
                            break;
                    }
                    }
                }

                if (eb.turn() && !s_ptt_this_press && !bender_ai_recording()) {
                    if (eb.pressing()) {
                        s_enc_hold_had_turn_while_pressed = true;
                        s_bt_forget_pair_hold_ready = false;
                        bender_ai_ptt_cancel();
                        // getClicks() при удержании = число уже завершённых кликов в серии:
                        // 0 — один клик + поворот; 1 — двойной; 2 — тройной (яркость); 3 — четверной (Wi‑Fi / Bluetooth).
                        switch (eb.getClicks()) {
                            case 0:
                                if (strcmp(g_audio_source, "bt") == 0) {
                                    if (eb.dir() > 0) {
                                        bt_audio_avrcp_next();
                                    } else if (eb.dir() < 0) {
                                        bt_audio_avrcp_previous();
                                    }
                                    break;
                                }
                                radioState.vol += eb.dir();
                                radioState.vol = constrain(radioState.vol, 0, RadioConfig::ampVolumeUiMax);
                                if (airplay_owns_speaker()) {
                                    airplay_encoder_vol_changed();
                                } else {
                                    apply_output_volume();
                                    syncWifiWithAudioSilence();
                                }
                                Serial.printf("[Vol] %d\n", (int)radioState.vol);
                                print_val('v', radioState.vol);
                                s_batt_matrix_overlay = false;
                                matrix_tmr.start(RadioConfig::matrixOverlayDigitsMs);
                                break;
                            case 1: {
                                const int8_t d = eb.dir();
                                int m = (int)radioState.mode + (int)d;
                                m = (m % 6 + 6) % 6;
                                radioState.mode = (uint8_t)m;
                                break;
                            }
                            case 2: {
                                const int8_t d = eb.dir();
                                int v = max((int)radioState.bright_mouth, (int)radioState.bright_eyes) + (int)d;
                                v = constrain(v, 0, matrix_base_max());
                                radioState.bright_mouth = (int8_t)v;
                                radioState.bright_eyes = (int8_t)v;
                                upd_bright();
                                break;
                            }
                            case 3: {
                                if (RadioConfig::airplayEnable && !RadioConfig::bluetoothEnable) {
                                    if (!s_mode_pick_active) {
                                        s_mode_pick_active = true;
                                        strncpy(s_mode_pick_choice, play_mode_is_airplay() ? "radio" : "ap",
                                                sizeof(s_mode_pick_choice) - 1);
                                        s_mode_pick_choice[sizeof(s_mode_pick_choice) - 1] = '\0';
                                    } else {
                                        if (strcmp(s_mode_pick_choice, "ap") == 0) {
                                            strncpy(s_mode_pick_choice, "radio", sizeof(s_mode_pick_choice));
                                        } else {
                                            strncpy(s_mode_pick_choice, "ap", sizeof(s_mode_pick_choice));
                                        }
                                        s_mode_pick_choice[sizeof(s_mode_pick_choice) - 1] = '\0';
                                    }
                                    break;
                                }
                                if (!RadioConfig::bluetoothEnable) {
                                    break;
                                }
                                if (!s_mode_pick_active) {
                                    s_mode_pick_active = true;
                                    if (strcmp(g_audio_source, "bt") == 0) {
                                        strncpy(s_mode_pick_choice, "wifi", sizeof(s_mode_pick_choice));
                                    } else {
                                        strncpy(s_mode_pick_choice, "bt", sizeof(s_mode_pick_choice));
                                    }
                                    s_mode_pick_choice[sizeof(s_mode_pick_choice) - 1] = '\0';
                                } else {
                                    if (strcmp(s_mode_pick_choice, "wifi") == 0) {
                                        strncpy(s_mode_pick_choice, "bt", sizeof(s_mode_pick_choice));
                                    } else {
                                        strncpy(s_mode_pick_choice, "wifi", sizeof(s_mode_pick_choice));
                                    }
                                    s_mode_pick_choice[sizeof(s_mode_pick_choice) - 1] = '\0';
                                }
                                break;
                            }
                            default:
                                break;
                        }
                    } else {
                        if ((radioState.state || bender_ai_owns_speaker() || airplay_owns_speaker()) &&
                            RadioConfig::encoderControlsVolume) {
                            angry_tmr.start();
                            radioState.vol += eb.dir();
                            radioState.vol = constrain(radioState.vol, 0, RadioConfig::ampVolumeUiMax);
                            if (airplay_owns_speaker()) {
                                airplay_encoder_vol_changed();
                            }
                            apply_output_volume();
                            syncWifiWithAudioSilence();
                            Serial.printf("[Vol] %d\n", (int)radioState.vol);
                            print_val('v', radioState.vol);
                            s_batt_matrix_overlay = false;
                            matrix_tmr.start(RadioConfig::matrixOverlayDigitsMs);
                        }
                    }
                }

                if (eb.release()) {
                    if (s_bt_forget_pair_hold_ready) {
                        s_bt_forget_pair_hold_ready = false;
                        if (strcmp(g_audio_source, "bt") == 0) {
                            bt_audio_forget_paired_devices();
                        }
                    }
                    if (s_mode_pick_active) {
                        s_mode_pick_active = false;
                        if (strcmp(s_mode_pick_choice, "ap") == 0 ||
                            strcmp(s_mode_pick_choice, "radio") == 0) {
                            commitPlayModeSwitch(s_mode_pick_choice);
                        } else if (strcmp(s_mode_pick_choice, g_audio_source) != 0) {
                            Serial.printf("[Mode] switch to %s\n", s_mode_pick_choice);
                            commitSourceModeSwitch(s_mode_pick_choice);
                        }
                    }
                    if (station_changed) {
                        station_changed = 0;
                        reconnect = station_url_by_index(radioState.station);
                        Serial.printf("[Radio] switch → station %d %s\n", (int)radioState.station,
                                      reconnect ? reconnect : "(null)");
                    }
                    s_bt_forget_pair_hold_ready = false;
                }
                memory.update();
            }
        }
        }

        syncWifiWithAudioSilence();

        {
            const bool ap_up = (WiFi.getMode() == WIFI_AP || WiFi.getMode() == WIFI_AP_STA);
            const bool charging = RadioConfig::chargingDetectEnable && battery_is_charging();
            const bool stay_awake =
                radioState.state || bender_ai_busy() || airplay_playing() || play_mode_is_airplay() ||
                    pong_active() ||
                s_mode_pick_active || wifiConnecting || show_wake_after_sleep_anim || ap_up ||
                charging || (strcmp(g_audio_source, "bt") == 0 && bt_audio_needs_pairing_ui());
            if (stay_awake) {
                s_wifi_last_activity_ms = millis();
            } else if (s_face_calm_after_ms > 0 && RadioConfig::benderIdleDeepSleepMs > 0 &&
                       (uint32_t)(millis() - s_wifi_last_activity_ms) >=
                           RadioConfig::benderIdleDeepSleepMs) {
                Serial.println(F("[Sleep] idle 30 min"));
                radio_enter_deep_sleep();
            }
        }

        if (RadioConfig::core0LoopDelayMs > 0) {
            delay(RadioConfig::core0LoopDelayMs);
        }
    }
}
