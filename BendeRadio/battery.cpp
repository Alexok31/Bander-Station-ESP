#include "battery.h"

#include <Arduino.h>

#include "RadioConfig.h"
#include "core0.h"

extern Data data;

static uint32_t s_last_sample_ms;
static uint16_t s_smooth_mv;
static uint8_t s_percent;
static bool s_gauge_ready;

static uint32_t s_charging_read_ms;
static bool s_charging_cached;

// Таблиця: мВ пакета (2S, після BMS) → %; обидва рядки по зростанню U. Без інтерполяції:
// береться останній %, для якого U ≥ порога (ступінчасто).
static const uint16_t batterySocTableMv[] = {
    6000, 6150, 6300, 6500, 6700, 6900, 7100, 7300, 7500, 7700, 7900, 8050, 8200, 8350, 8500,
};
static const uint8_t batterySocTablePct[] = {
    0, 3, 7, 12, 18, 25, 33, 42, 50, 58, 68, 76, 84, 92, 99,
};
static constexpr uint8_t batterySocTableN =
    (uint8_t)(sizeof(batterySocTableMv) / sizeof(batterySocTableMv[0]));
static_assert(sizeof(batterySocTableMv) / sizeof(batterySocTableMv[0]) ==
                  sizeof(batterySocTablePct) / sizeof(batterySocTablePct[0]),
              "batterySocTable mv/pct count mismatch");

static uint8_t percent_from_pack_mv(uint16_t pack_mv) {
    if (batterySocTableN == 0u) {
        return 0u;
    }
    uint8_t out = batterySocTablePct[0];
    for (uint8_t i = 0; i < batterySocTableN; i++) {
        if (pack_mv >= batterySocTableMv[i]) {
            out = batterySocTablePct[i];
        } else {
            break;
        }
    }
    return out;
}

static uint32_t adc_pin_millivolts() {
#if defined(ESP_ARDUINO_VERSION_MAJOR) && (ESP_ARDUINO_VERSION_MAJOR >= 2)
    return (uint32_t)analogReadMilliVolts(RadioConfig::batteryAdcPin);
#else
    const uint32_t raw = analogRead(RadioConfig::batteryAdcPin);
    return (uint32_t)((uint64_t)raw * 3300u / 4095u);
#endif
}

static bool charging_pin_majority_high() {
    if (!RadioConfig::chargingDetectEnable) {
        return false;
    }
    uint8_t n = 0;
    for (uint8_t i = 0; i < 8u; i++) {
        if (digitalRead(RadioConfig::chargingDetectPin) == HIGH) {
            n++;
        }
    }
    return n >= 5u;
}

void battery_init() {
    s_last_sample_ms = 0;
    s_smooth_mv = 0;
    s_percent = 0;
    s_gauge_ready = false;
    s_charging_read_ms = 0;
    s_charging_cached = false;
    if (RadioConfig::chargingDetectEnable) {
        pinMode(RadioConfig::chargingDetectPin, INPUT);
    }
    if (!RadioConfig::batteryMonitorEnable) {
        return;
    }
    pinMode(RadioConfig::batteryAdcPin, INPUT);
    analogSetPinAttenuation(RadioConfig::batteryAdcPin, ADC_11db);
}

static void battery_sample_apply() {
    const bool chg = charging_pin_majority_high();

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

    int32_t p = (int32_t)percent_from_pack_mv(s_smooth_mv);
    if (p < 0) {
        p = 0;
    }
    if (p > 99) {
        p = 99;
    }
    s_percent = (uint8_t)p;
    s_charging_cached = chg;
    s_charging_read_ms = millis();
    s_gauge_ready = true;
}

void battery_force_sample() {
    if (!RadioConfig::batteryMonitorEnable) {
        return;
    }
    battery_sample_apply();
    s_last_sample_ms = millis();
}

bool battery_gauge_ready() {
    return RadioConfig::batteryMonitorEnable && s_gauge_ready;
}

bool battery_update() {
    if (!RadioConfig::batteryMonitorEnable) {
        return false;
    }
    const uint32_t now = millis();
    uint32_t interval = (!data.state && RadioConfig::batterySampleIntervalIdleMs > 0)
                            ? RadioConfig::batterySampleIntervalIdleMs
                            : RadioConfig::batterySampleIntervalMs;
    if (s_gauge_ready && s_percent < RadioConfig::batteryLowAttentionPercent) {
        if (RadioConfig::batteryLowSampleIntervalMs > 0 &&
            interval > RadioConfig::batteryLowSampleIntervalMs) {
            interval = RadioConfig::batteryLowSampleIntervalMs;
        }
    }
    if ((uint32_t)(now - s_last_sample_ms) < interval) {
        return false;
    }
    s_last_sample_ms = now;
    battery_sample_apply();
    return true;
}

uint8_t battery_percent() {
    return s_percent;
}

uint16_t battery_millivolts() {
    return s_smooth_mv;
}

bool battery_is_charging() {
    if (!RadioConfig::chargingDetectEnable) {
        return false;
    }
    const uint32_t now = millis();
    if (s_charging_read_ms == 0u || (uint32_t)(now - s_charging_read_ms) >= 80u) {
        s_charging_read_ms = now;
        s_charging_cached = charging_pin_majority_high();
    }
    return s_charging_cached;
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
