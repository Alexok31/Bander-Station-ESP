#include "BtAudio.h"

#include "RadioConfig.h"
#include "pcm_analyzer.h"

#include <BluetoothA2DPSink.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <esp_avrc_api.h>
#include <esp_idf_version.h>

static BluetoothA2DPSink g_a2dp_sink;
static bool g_sink_running = false;

static volatile uint32_t s_bt_track_duration_ms = 0;
// Якорь для позиции: AVRCP play_pos обновляет редко или не приходит — при PLAYING добавляем (millis − anchor_wall).
static volatile uint32_t s_bt_anchor_pos_ms = 0;
static volatile uint32_t s_bt_anchor_wall_ms = 0;
static volatile uint8_t s_bt_is_playing = 0;
// После перемотки (AVRCP seek / next / prev) не экстраполировать, пока не придёт play_pos или таймаут.
static volatile uint8_t s_bt_pos_resync_pending = 0;
static volatile uint32_t s_bt_pos_resync_deadline_ms = 0;
static volatile uint8_t s_bt_prev_playstat_u8 = 0xff;
static volatile uint32_t s_bt_last_play_pos_rx_ms = 0u;
static volatile uint32_t s_bt_last_play_pos_poll_ms = 0u;
// До этой метки millis() чаще опрашиваем RN PLAY_POS (перемотка / resync).
static uint32_t s_bt_pos_poll_burst_until_ms = 0u;

static char s_bt_title[96];
static char s_bt_artist[96];
static volatile uint32_t s_bt_meta_serial = 0u;

static void bt_meta_bump_serial() {
    s_bt_meta_serial++;
}

// Латиница a–z и типичная кириллица UTF‑8 (Ё/ё) → верхний регистр для бегущей строки.
static void bt_fold_upper_in_place(char* s) {
    auto* p = reinterpret_cast<unsigned char*>(s);
    while (*p != 0u) {
        if (*p >= 'a' && *p <= 'z') {
            *p = static_cast<unsigned char>(*p - 32u);
            ++p;
            continue;
        }
        // а–п : D0 B0–BF → D0 90–AF
        if (*p == 0xD0u && p[1] >= 0xB0u && p[1] <= 0xBFu) {
            p[1] = static_cast<unsigned char>(p[1] - 0x20u);
            p += 2;
            continue;
        }
        // р–я : D1 80–8F → D0 A0–AF
        if (*p == 0xD1u && p[1] >= 0x80u && p[1] <= 0x8Fu) {
            *p = 0xD0u;
            p[1] = static_cast<unsigned char>(p[1] + 0x20u);
            p += 2;
            continue;
        }
        // ё D1 91 → Ё D0 81
        if (*p == 0xD1u && p[1] == 0x91u) {
            p[0] = 0xD0u;
            p[1] = 0x81u;
            p += 2;
            continue;
        }
        if ((*p & 0xE0u) == 0xC0u) {
            p += 2;
            continue;
        }
        if ((*p & 0xF0u) == 0xE0u) {
            p += 3;
            continue;
        }
        if ((*p & 0xF8u) == 0xF0u) {
            p += 4;
            continue;
        }
        ++p;
    }
}

static void bt_copy_meta_text(char* dst, size_t cap, const uint8_t* text) {
    if (cap == 0u) {
        return;
    }
    if (text == nullptr) {
        dst[0] = '\0';
        return;
    }
    strncpy(dst, reinterpret_cast<const char*>(text), cap - 1u);
    dst[cap - 1u] = '\0';
    for (size_t i = 0; dst[i] != '\0'; i++) {
        const char c = dst[i];
        if (c == '\r' || c == '\n' || c == '\t') {
            dst[i] = ' ';
        }
    }
    bt_fold_upper_in_place(dst);
}

static bool bt_playstat_is_seek(esp_avrc_playback_stat_t st) {
    return st == ESP_AVRC_PLAYBACK_FWD_SEEK || st == ESP_AVRC_PLAYBACK_REV_SEEK;
}

static void bt_avrc_play_pos_cb(uint32_t play_pos_ms);
void bt_audio_poll_track_position();

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(4, 0, 0)
static void bt_avrcp_notify_play_pos_changed(uint32_t min_gap_ms) {
    if (!g_sink_running || !g_a2dp_sink.is_connected() || !g_a2dp_sink.is_avrc_connected()) {
        return;
    }
    const uint32_t now = millis();
    if (min_gap_ms != 0u && (uint32_t)(now - s_bt_last_play_pos_poll_ms) < min_gap_ms) {
        return;
    }
    s_bt_last_play_pos_poll_ms = now;
    (void)esp_avrc_ct_send_register_notification_cmd(APP_RC_CT_TL_RN_PLAY_POS_CHANGE,
                                                     (uint8_t)ESP_AVRC_RN_PLAY_POS_CHANGED, 1u);
}
#else
static void bt_avrcp_notify_play_pos_changed(uint32_t min_gap_ms) {
    (void)min_gap_ms;
}
#endif

static void bt_request_pos_resync() {
    s_bt_pos_resync_pending = 1u;
    s_bt_pos_resync_deadline_ms = millis() + 10000u;
    s_bt_pos_poll_burst_until_ms = millis() + 12000u;
    g_a2dp_sink.set_avrc_rn_play_pos_callback(bt_avrc_play_pos_cb, 1);
    bt_avrcp_notify_play_pos_changed(0u);
}

static void bt_avrc_metadata_cb(uint8_t attr_id, const uint8_t* text) {
    if (attr_id == (uint8_t)ESP_AVRC_MD_ATTR_PLAYING_TIME && text != nullptr) {
        const uint32_t v = (uint32_t)strtoul(reinterpret_cast<const char*>(text), nullptr, 10);
        const uint32_t prev = s_bt_track_duration_ms;
        s_bt_track_duration_ms = v;
        if (prev != v) {
            s_bt_anchor_pos_ms = 0;
            s_bt_anchor_wall_ms = millis();
        }
        return;
    }
    if (attr_id == (uint8_t)ESP_AVRC_MD_ATTR_TITLE) {
        char tmp[sizeof(s_bt_title)];
        bt_copy_meta_text(tmp, sizeof(tmp), text);
        if (strcmp(tmp, s_bt_title) != 0) {
            strncpy(s_bt_title, tmp, sizeof(s_bt_title) - 1u);
            s_bt_title[sizeof(s_bt_title) - 1u] = '\0';
            bt_meta_bump_serial();
        }
        return;
    }
    if (attr_id == (uint8_t)ESP_AVRC_MD_ATTR_ARTIST) {
        char tmp[sizeof(s_bt_artist)];
        bt_copy_meta_text(tmp, sizeof(tmp), text);
        if (strcmp(tmp, s_bt_artist) != 0) {
            strncpy(s_bt_artist, tmp, sizeof(s_bt_artist) - 1u);
            s_bt_artist[sizeof(s_bt_artist) - 1u] = '\0';
            bt_meta_bump_serial();
        }
    }
}

static void bt_avrc_play_pos_cb(uint32_t play_pos_ms) {
    s_bt_anchor_pos_ms = play_pos_ms;
    const uint32_t now = millis();
    s_bt_anchor_wall_ms = now;
    s_bt_last_play_pos_rx_ms = now;
    s_bt_pos_resync_pending = 0u;
}

static void bt_freeze_extrapolation_to_anchor() {
    if (s_bt_is_playing == 0u) {
        return;
    }
    const uint32_t dur = s_bt_track_duration_ms;
    const uint32_t now = millis();
    uint64_t pos = (uint64_t)s_bt_anchor_pos_ms + (uint64_t)(now - s_bt_anchor_wall_ms);
    if (dur > 1u && pos >= dur) {
        pos = dur - 1u;
    }
    s_bt_anchor_pos_ms = (uint32_t)pos;
    s_bt_anchor_wall_ms = now;
}

static void bt_avrc_playstatus_cb(esp_avrc_playback_stat_t st) {
    const bool have_prev = (s_bt_prev_playstat_u8 != 0xffu);
    const esp_avrc_playback_stat_t prev =
        have_prev ? (esp_avrc_playback_stat_t)s_bt_prev_playstat_u8 : st;

    const bool was_extrap = (s_bt_is_playing != 0u);
    const bool is_extrap = (st == ESP_AVRC_PLAYBACK_PLAYING);
    const bool seek_now = bt_playstat_is_seek(st);
    const bool seek_prev = bt_playstat_is_seek(prev);

    if (was_extrap && seek_now) {
        // PLAYING → перемотка: зафиксировать текущую оценку на время seek
        bt_freeze_extrapolation_to_anchor();
        // Часть телефонов не шлёт FWD/REV_SEEK только в конце — опрос и во время scrub.
        s_bt_pos_poll_burst_until_ms = millis() + 15000u;
        bt_avrcp_notify_play_pos_changed(0u);
    } else if (was_extrap && !is_extrap && !seek_now) {
        // пауза / стоп
        bt_freeze_extrapolation_to_anchor();
    }

    if (seek_prev && is_extrap) {
        // конец перемотки → снова PLAYING: ждём реальную позицию с телефона
        s_bt_anchor_wall_ms = millis();
        bt_request_pos_resync();
    } else if (!was_extrap && is_extrap && !seek_prev) {
        // пауза → play (не после seek)
        s_bt_anchor_wall_ms = millis();
    }

    s_bt_is_playing = is_extrap ? 1u : 0u;
    s_bt_prev_playstat_u8 = (uint8_t)st;
}

static void bt_avrc_track_change_cb(uint8_t* /*uid*/) {
    s_bt_track_duration_ms = 0;
    s_bt_title[0] = '\0';
    s_bt_artist[0] = '\0';
    bt_meta_bump_serial();
    s_bt_anchor_pos_ms = 0;
    s_bt_anchor_wall_ms = millis();
    // На некоторых телефонах PLAY_STATUS после смены трека не приходит: не сбрасываем play-state в 0 принудительно.
    s_bt_pos_resync_pending = 1u;
    s_bt_pos_resync_deadline_ms = millis() + 10000u;
    s_bt_pos_poll_burst_until_ms = millis() + 12000u;
    s_bt_prev_playstat_u8 = 0xffu;
    bt_avrcp_notify_play_pos_changed(0u);
}

// До volume_control() в BluetoothA2DPSink — иначе после громкости телефона/синка уровень для анализа часто у нуля.
static void bt_a2dp_pcm_raw_cb(const uint8_t* data, uint32_t len) {
    pcm_analyzer_on_bt_pcm_bytes(data, len);
}

static uint32_t s_bt_next_reconnect_ms = 0;
static uint8_t s_bt_reconnect_attempts = 0;

void bt_audio_start_sink() {
    if (g_sink_running) {
        return;
    }
    s_bt_next_reconnect_ms = 0;
    s_bt_reconnect_attempts = 0;

    i2s_pin_config_t pin_config = {};
    pin_config.bck_io_num = RadioConfig::i2sBclk;
    pin_config.ws_io_num = RadioConfig::i2sLrc;
    pin_config.data_out_num = RadioConfig::i2sDout;
    pin_config.data_in_num = I2S_PIN_NO_CHANGE;

    g_a2dp_sink.set_pin_config(pin_config);
    g_a2dp_sink.set_reconnect_delay((int)RadioConfig::btA2dpLastConnPreStackDelayMs);
    g_a2dp_sink.set_raw_stream_reader(bt_a2dp_pcm_raw_cb);
    g_a2dp_sink.set_avrc_metadata_attribute_mask(ESP_AVRC_MD_ATTR_TITLE | ESP_AVRC_MD_ATTR_ARTIST |
                                                 ESP_AVRC_MD_ATTR_ALBUM | ESP_AVRC_MD_ATTR_TRACK_NUM |
                                                 ESP_AVRC_MD_ATTR_NUM_TRACKS | ESP_AVRC_MD_ATTR_GENRE |
                                                 ESP_AVRC_MD_ATTR_PLAYING_TIME);
    g_a2dp_sink.set_avrc_metadata_callback(bt_avrc_metadata_cb);
    g_a2dp_sink.set_avrc_rn_play_pos_callback(bt_avrc_play_pos_cb, 1);
    g_a2dp_sink.set_avrc_rn_playstatus_callback(bt_avrc_playstatus_cb);
    g_a2dp_sink.set_avrc_rn_track_change_callback(bt_avrc_track_change_cb);
    // Второй аргумент true — autoreconnect + last BDA из NVS (см. BluetoothA2DPSink::start(name, bool)).
    g_a2dp_sink.start(RadioConfig::btSinkName, true);
    g_sink_running = true;
}

void bt_audio_stop_sink() {
    if (!g_sink_running) {
        return;
    }
    g_a2dp_sink.set_avrc_metadata_callback(nullptr);
    g_a2dp_sink.set_avrc_rn_play_pos_callback(nullptr, 1);
    g_a2dp_sink.set_avrc_rn_playstatus_callback(nullptr);
    g_a2dp_sink.set_avrc_rn_track_change_callback(nullptr);
    s_bt_track_duration_ms = 0;
    s_bt_anchor_pos_ms = 0;
    s_bt_anchor_wall_ms = 0;
    s_bt_is_playing = 0;
    s_bt_pos_resync_pending = 0u;
    s_bt_prev_playstat_u8 = 0xffu;
    s_bt_last_play_pos_rx_ms = 0u;
    s_bt_last_play_pos_poll_ms = 0u;
    s_bt_pos_poll_burst_until_ms = 0u;
    s_bt_title[0] = '\0';
    s_bt_artist[0] = '\0';
    bt_meta_bump_serial();
    g_a2dp_sink.set_raw_stream_reader(nullptr);
    g_a2dp_sink.set_stream_reader(nullptr, true);
    g_a2dp_sink.end(true);
    g_sink_running = false;
    s_bt_next_reconnect_ms = 0;
    s_bt_reconnect_attempts = 0;
}

bool bt_audio_is_sink_running() {
    return g_sink_running;
}

bool bt_audio_needs_pairing_ui() {
    if (!g_sink_running) {
        return false;
    }
    return !g_a2dp_sink.is_connected();
}

void bt_audio_tick() {
    if (!g_sink_running) {
        return;
    }
    if (g_a2dp_sink.is_connected()) {
        s_bt_reconnect_attempts = 0;
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(4, 0, 0)
        if (g_a2dp_sink.is_avrc_connected() && s_bt_track_duration_ms > 1u) {
            const uint32_t now = millis();
            const bool burst =
                ((int32_t)(s_bt_pos_poll_burst_until_ms - now) > 0) || (s_bt_pos_resync_pending != 0u);
            if (burst) {
                bt_avrcp_notify_play_pos_changed(400u);
            } else if (s_bt_is_playing != 0u &&
                       (uint32_t)(now - s_bt_last_play_pos_rx_ms) >= 4000u) {
                // Телефон мог «тихо» перемотать в состоянии PLAYING без FWD/REV_SEEK.
                bt_avrcp_notify_play_pos_changed(2000u);
            }
        }
#endif
        return;
    }
    const uint32_t now = millis();
    if (s_bt_next_reconnect_ms == 0) {
        s_bt_next_reconnect_ms = now + RadioConfig::btReconnectFirstDelayMs;
    }
    if ((int32_t)(now - s_bt_next_reconnect_ms) < 0) {
        return;
    }
    (void)g_a2dp_sink.reconnect();
    s_bt_reconnect_attempts++;
    if (s_bt_reconnect_attempts < RadioConfig::btReconnectBurstCount) {
        s_bt_next_reconnect_ms = now + RadioConfig::btReconnectRetryMs;
    } else {
        s_bt_next_reconnect_ms = now + 45000u;
    }
}

void bt_audio_volume_apply(bool audio_on, int8_t vol_ui) {
    if (!g_sink_running) {
        return;
    }
    if (!audio_on || vol_ui <= 0) {
        g_a2dp_sink.set_volume(0);
        return;
    }
    uint8_t v = (uint8_t)(((int)vol_ui * 127 + 10) / 21);
    if (v > 127) {
        v = 127;
    }
    g_a2dp_sink.set_volume(v);
}

void bt_audio_avrcp_pause() {
    if (!g_sink_running || !g_a2dp_sink.is_connected() || !g_a2dp_sink.is_avrc_connected()) {
        return;
    }
    g_a2dp_sink.pause();
    bt_freeze_extrapolation_to_anchor();
    s_bt_is_playing = 0;
    s_bt_pos_resync_pending = 0u;
}

void bt_audio_avrcp_play() {
    if (!g_sink_running || !g_a2dp_sink.is_connected() || !g_a2dp_sink.is_avrc_connected()) {
        return;
    }
    g_a2dp_sink.play();
    s_bt_is_playing = 1;
    s_bt_anchor_wall_ms = millis();
    s_bt_pos_resync_pending = 0u;
}

void bt_audio_avrcp_next() {
    if (!g_sink_running || !g_a2dp_sink.is_connected() || !g_a2dp_sink.is_avrc_connected()) {
        return;
    }
    if (s_bt_is_playing != 0u) {
        bt_freeze_extrapolation_to_anchor();
    }
    g_a2dp_sink.next();
    bt_request_pos_resync();
}

void bt_audio_avrcp_previous() {
    if (!g_sink_running || !g_a2dp_sink.is_connected() || !g_a2dp_sink.is_avrc_connected()) {
        return;
    }
    if (s_bt_is_playing != 0u) {
        bt_freeze_extrapolation_to_anchor();
    }
    g_a2dp_sink.previous();
    bt_request_pos_resync();
}

uint32_t bt_audio_track_duration_ms() {
    return s_bt_track_duration_ms;
}

void bt_audio_poll_track_position() {
    bt_avrcp_notify_play_pos_changed(1500u);
}

uint32_t bt_audio_track_meta_serial() {
    return s_bt_meta_serial;
}

const char* bt_audio_track_scroll_cstr() {
    if (s_bt_title[0] == '\0' && s_bt_artist[0] == '\0') {
        return "BT";
    }
    if (s_bt_artist[0] == '\0') {
        return s_bt_title;
    }
    if (s_bt_title[0] == '\0') {
        return s_bt_artist;
    }
    static char line[sizeof(s_bt_title) + sizeof(s_bt_artist) + 8u];
    (void)snprintf(line, sizeof(line), "%s  |  %s", s_bt_title, s_bt_artist);
    line[sizeof(line) - 1u] = '\0';
    return line;
}

uint32_t bt_audio_track_position_ms() {
    const uint32_t dur = s_bt_track_duration_ms;
    if (dur <= 1u) {
        return 0;
    }
    if (s_bt_pos_resync_pending != 0u) {
        const uint32_t now = millis();
        if ((int32_t)(now - s_bt_pos_resync_deadline_ms) >= 0) {
            s_bt_pos_resync_pending = 0u;
        } else if (s_bt_is_playing != 0u) {
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(4, 0, 0)
            bt_avrcp_notify_play_pos_changed(400u);
#endif
            uint32_t p = s_bt_anchor_pos_ms;
            if (p >= dur) {
                p = dur - 1u;
            }
            return p;
        }
    }
    if (s_bt_is_playing == 0u) {
        uint32_t p = s_bt_anchor_pos_ms;
        if (p >= dur) {
            p = dur - 1u;
        }
        return p;
    }
    const uint32_t now = millis();
    if ((uint32_t)(now - s_bt_last_play_pos_rx_ms) >= 3500u) {
        bt_audio_poll_track_position();
    }
    uint64_t pos = (uint64_t)s_bt_anchor_pos_ms + (uint64_t)(now - s_bt_anchor_wall_ms);
    if (pos >= dur) {
        return dur - 1u;
    }
    return (uint32_t)pos;
}
