#pragma once

#include <Arduino.h>

// Аналоговый мультиплексор CD4052BE (питание 5 V, VEE/VSS/INH/B = GND).
// PCM5100 L/R -> X0/Y0; усилитель INL/INR <- X/Y (pin 13/3).
// BK8000L AUDIOLP/AUDIORP -> X1 pin 14 / Y1 pin 5; A=HIGH -> усилитель.
enum class AudioSource : uint8_t {
    Wifi = 0,       // CD4052 A = LOW  -> канал 0 (PCM5100)
    Bluetooth = 1,  // CD4052 A = HIGH -> канал 1 (BK8000L)
};

// initial — из NVS (wifi → GPIO LOW / PCM5100).
void audio_mux_init(AudioSource initial = AudioSource::Wifi);
void audio_mux_select(AudioSource source);
AudioSource audio_mux_get();

// Совместимость с NVS/UI: "wifi" | "bt"
AudioSource audio_source_from_string(const char* aud);
const char* audio_source_to_string(AudioSource source);
void audio_mux_apply_legacy_string(const char* aud);
