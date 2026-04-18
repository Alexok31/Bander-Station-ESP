#include "BtAudio.h"

#include "RadioConfig.h"

#include <BluetoothA2DPSink.h>

static BluetoothA2DPSink g_a2dp_sink;
static bool g_sink_running = false;

void bt_audio_start_sink() {
    if (g_sink_running) {
        return;
    }
    i2s_pin_config_t pin_config = {};
    pin_config.bck_io_num = RadioConfig::i2sBclk;
    pin_config.ws_io_num = RadioConfig::i2sLrc;
    pin_config.data_out_num = RadioConfig::i2sDout;
    pin_config.data_in_num = I2S_PIN_NO_CHANGE;

    g_a2dp_sink.set_pin_config(pin_config);
    g_a2dp_sink.start(RadioConfig::btSinkName);
    g_sink_running = true;
}

void bt_audio_stop_sink() {
    if (!g_sink_running) {
        return;
    }
    g_a2dp_sink.end(true);
    g_sink_running = false;
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
