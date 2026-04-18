#include "battery.h"

#include <Arduino.h>

#include "RadioConfig.h"
#include "core0.h"

extern Data data;

static uint32_t s_last_sample_ms;
static uint16_t s_smooth_mv;
static uint8_t s_percent;

static uint32_t adc_pin_millivolts() {
#if defined(ESP_ARDUINO_VERSION_MAJOR) && (ESP_ARDUINO_VERSION_MAJOR >= 2)
    return (uint32_t)analogReadMilliVolts(RadioConfig::batteryAdcPin);
#else
    const uint32_t raw = analogRead(RadioConfig::batteryAdcPin);
    return (uint32_t)((uint64_t)raw * 3300u / 4095u);
#endif
}

void battery_init() {
    s_last_sample_ms = 0;
    s_smooth_mv = 0;
    s_percent = 0;
    if (!RadioConfig::batteryMonitorEnable) {
        return;
    }
    pinMode(RadioConfig::batteryAdcPin, INPUT);
    analogSetPinAttenuation(RadioConfig::batteryAdcPin, ADC_11db);
}

static void battery_sample_apply() {
    uint32_t acc = 0;
    constexpr uint8_t kSamples = 12;
    for (uint8_t i = 0; i < kSamples; i++) {
        acc += adc_pin_millivolts();
    }
    const uint32_t pin_mv = acc / kSamples;
    const float ratio = RadioConfig::batteryDividerRatio;
    uint32_t pack_mv = (uint32_t)((float)pin_mv * ratio + 0.5f);
    if (pack_mv > 20000u) {
        pack_mv = 20000u;
    }

    if (s_smooth_mv == 0) {
        s_smooth_mv = (uint16_t)pack_mv;
    } else {
        const uint32_t ema = (uint32_t)s_smooth_mv * 7u + pack_mv;
        s_smooth_mv = (uint16_t)(ema / 8u);
    }

    const int32_t lo = (int32_t)RadioConfig::batteryEmptyMv;
    const int32_t hi = (int32_t)RadioConfig::batteryFullMv;
    int32_t p = 0;
    if (hi > lo) {
        // 0…99 %: на двух цифрах матрицы 100% виглядало як «00» (десятки 10 → некоректний гліф).
        p = ((int32_t)s_smooth_mv - lo) * 99 / (hi - lo);
    }
    if (p < 0) {
        p = 0;
    }
    if (p > 99) {
        p = 99;
    }
    s_percent = (uint8_t)p;
}

void battery_force_sample() {
    if (!RadioConfig::batteryMonitorEnable) {
        return;
    }
    battery_sample_apply();
    s_last_sample_ms = millis();
}

void battery_update() {
    if (!RadioConfig::batteryMonitorEnable) {
        return;
    }
    const uint32_t now = millis();
    const uint32_t interval = (!data.state && RadioConfig::batterySampleIntervalIdleMs > 0)
                                  ? RadioConfig::batterySampleIntervalIdleMs
                                  : RadioConfig::batterySampleIntervalMs;
    if ((uint32_t)(now - s_last_sample_ms) < interval) {
        return;
    }
    s_last_sample_ms = now;
    battery_sample_apply();
}

uint8_t battery_percent() {
    return s_percent;
}

uint16_t battery_millivolts() {
    return s_smooth_mv;
}

uint8_t battery_eye_mood() {
    if (!RadioConfig::batteryMonitorEnable) {
        return 1u;
    }
    const uint8_t p = s_percent;
    if (p >= RadioConfig::batteryMoodCheerfulMinPct) {
        return 0u;
    }
    if (p >= RadioConfig::batteryMoodNormalMinPct) {
        return 1u;
    }
    return 2u;
}
