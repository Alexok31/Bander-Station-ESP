#include "AudioMux.h"

#include "RadioConfig.h"

#include <cstring>

static AudioSource s_mux_source = AudioSource::Wifi;

static uint8_t audio_mux_gpio_level_for(AudioSource source) {
    const bool pcm_channel = (source == AudioSource::Wifi);
    return (pcm_channel == RadioConfig::audioMuxWifiIsGpioLow) ? LOW : HIGH;
}

void audio_mux_init(AudioSource initial) {
    if (RadioConfig::audioMuxSelectPin == 255) {
        s_mux_source = AudioSource::Wifi;
        return;
    }
    pinMode(RadioConfig::audioMuxSelectPin, OUTPUT);
    audio_mux_select(initial);
}

void audio_mux_select(AudioSource source) {
    s_mux_source = source;
    if (RadioConfig::audioMuxSelectPin == 255) {
        return;
    }
    const uint8_t level = audio_mux_gpio_level_for(source);
    digitalWrite(RadioConfig::audioMuxSelectPin, level);
    delayMicroseconds(RadioConfig::audioMuxSettleUs);
}

AudioSource audio_mux_get() {
    return s_mux_source;
}

AudioSource audio_source_from_string(const char* aud) {
    if (RadioConfig::bluetoothEnable && aud != nullptr && strcmp(aud, "bt") == 0) {
        return AudioSource::Bluetooth;
    }
    return AudioSource::Wifi;
}

const char* audio_source_to_string(AudioSource source) {
    return (source == AudioSource::Bluetooth) ? "bt" : "wifi";
}

void audio_mux_apply_legacy_string(const char* aud) {
    audio_mux_select(audio_source_from_string(aud));
}
