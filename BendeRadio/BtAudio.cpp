#include "BtAudio.h"

#include "RadioConfig.h"

#include <BluetoothA2DPSink.h>
#include <esp_avrc_api.h>
#include <vector>

static BluetoothA2DPSink g_a2dp_sink;
static bool g_sink_running = false;

// Последний статус с телефона (AVRCP RN): -1 ещё неизвестно, 0 не играет, 1 играет.
static volatile int8_t g_bt_avrcp_playing = -1;
// Последнее значение, уже применённое к UI (чтобы не драться с оптимистичным нажатием энкодера).
static int8_t s_bt_applied_playing = -1;

static void bt_avrcp_playstatus_cb(esp_avrc_playback_stat_t playback) {
    switch (playback) {
        case ESP_AVRC_PLAYBACK_PLAYING:
            g_bt_avrcp_playing = 1;
            break;
        case ESP_AVRC_PLAYBACK_PAUSED:
        case ESP_AVRC_PLAYBACK_STOPPED:
            g_bt_avrcp_playing = 0;
            break;
        default:
            break;
    }
}

static void bt_a2dp_connection_cb(esp_a2d_connection_state_t state, void* /*obj*/) {
    if (state != ESP_A2D_CONNECTION_STATE_CONNECTED) {
        g_bt_avrcp_playing = -1;
        s_bt_applied_playing = -1;
    }
}

void bt_audio_start_sink() {
    if (g_sink_running) {
        return;
    }
    g_bt_avrcp_playing = -1;
    s_bt_applied_playing = -1;

    i2s_pin_config_t pin_config = {};
    pin_config.bck_io_num = RadioConfig::i2sBclk;
    pin_config.ws_io_num = RadioConfig::i2sLrc;
    pin_config.data_out_num = RadioConfig::i2sDout;
    pin_config.data_in_num = I2S_PIN_NO_CHANGE;

    g_a2dp_sink.set_pin_config(pin_config);
    g_a2dp_sink.set_on_connection_state_changed(bt_a2dp_connection_cb, nullptr);
    g_a2dp_sink.set_avrc_rn_events(
        std::vector<esp_avrc_rn_event_ids_t>{ESP_AVRC_RN_PLAY_STATUS_CHANGE, ESP_AVRC_RN_VOLUME_CHANGE});
    g_a2dp_sink.set_avrc_rn_playstatus_callback(bt_avrcp_playstatus_cb);
    g_a2dp_sink.start(RadioConfig::btSinkName);
    g_sink_running = true;
}

void bt_audio_stop_sink() {
    if (!g_sink_running) {
        return;
    }
    g_a2dp_sink.end(true);
    g_sink_running = false;
    g_bt_avrcp_playing = -1;
    s_bt_applied_playing = -1;
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

bool bt_audio_consume_remote_playstate(bool* new_playing) {
    if (!g_sink_running || !g_a2dp_sink.is_connected()) {
        s_bt_applied_playing = -1;
        return false;
    }
    const int8_t st = g_bt_avrcp_playing;
    if (st < 0) {
        return false;
    }
    if (st == s_bt_applied_playing) {
        return false;
    }
    s_bt_applied_playing = st;
    *new_playing = (st == 1);
    return true;
}

void bt_audio_avrcp_play() {
    if (!g_sink_running || !g_a2dp_sink.is_avrc_connected()) {
        return;
    }
    g_a2dp_sink.play();
}

void bt_audio_avrcp_pause() {
    if (!g_sink_running || !g_a2dp_sink.is_avrc_connected()) {
        return;
    }
    g_a2dp_sink.pause();
}

bool bt_audio_is_avrc_connected() {
    return g_sink_running && g_a2dp_sink.is_connected() && g_a2dp_sink.is_avrc_connected();
}
