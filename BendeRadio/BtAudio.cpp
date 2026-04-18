#include "BtAudio.h"

#include "RadioConfig.h"
#include "pcm_analyzer.h"

#include <BluetoothA2DPSink.h>

static BluetoothA2DPSink g_a2dp_sink;
static bool g_sink_running = false;

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
    // Второй аргумент true — autoreconnect + last BDA из NVS (см. BluetoothA2DPSink::start(name, bool)).
    g_a2dp_sink.start(RadioConfig::btSinkName, true);
    g_sink_running = true;
}

void bt_audio_stop_sink() {
    if (!g_sink_running) {
        return;
    }
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
}

void bt_audio_avrcp_play() {
    if (!g_sink_running || !g_a2dp_sink.is_connected() || !g_a2dp_sink.is_avrc_connected()) {
        return;
    }
    g_a2dp_sink.play();
}
