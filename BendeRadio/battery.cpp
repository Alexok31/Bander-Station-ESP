#include "battery.h"

#include <Arduino.h>

#include "RadioConfig.h"
#include "core0.h"

extern Data radioState;

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

static bool charging_detect_uses_usb_phy_pin() {
    const uint8_t p = RadioConfig::chargingDetectPin;
    return p == 19u || p == 20u;
}

static uint16_t charging_pin_millivolts() {
#if defined(ESP_ARDUINO_VERSION_MAJOR) && (ESP_ARDUINO_VERSION_MAJOR >= 2)
    return (uint16_t)analogReadMilliVolts(RadioConfig::chargingDetectPin);
#else
    const uint32_t raw = analogRead(RadioConfig::chargingDetectPin);
    return (uint16_t)((uint64_t)raw * 3300u / 4095u);
#endif
}

static bool charging_pin_majority_high() {
    if (!RadioConfig::chargingDetectEnable || charging_detect_uses_usb_phy_pin()) {
        return false;
    }
    uint32_t acc = 0;
    uint16_t peak = 0;
    uint8_t hits = 0;
    constexpr uint8_t kSamples = 16;
    for (uint8_t i = 0; i < kSamples; i++) {
        uint16_t mv = 0;
        if (RadioConfig::chargingDetectUseAdc) {
            mv = charging_pin_millivolts();
        } else if (digitalRead(RadioConfig::chargingDetectPin) == HIGH) {
            mv = 3300u;
        }
        acc += mv;
        if (mv > peak) {
            peak = mv;
        }
        if (mv >= RadioConfig::chargingDetectMinMv) {
            hits++;
        }
        delayMicroseconds(200);
    }
    if (RadioConfig::chargingDetectUseAdc) {
        return hits >= 3u && peak >= RadioConfig::chargingDetectMinMv;
    }
    const bool majority = acc >= (3300u * 5u);
    return RadioConfig::chargingDetectActiveHigh ? majority : !majority;
}

void battery_init() {
    s_last_sample_ms = 0;
    s_smooth_mv = 0;
    s_percent = 0;
    s_gauge_ready = false;
    s_charging_read_ms = 0;
    s_charging_cached = false;
    if (RadioConfig::chargingDetectEnable) {
        if (charging_detect_uses_usb_phy_pin()) {
            Serial.printf(
                "[Batt] SKIP charge pin GPIO%u — USB D+/D-, would kill Serial.\n",
                (unsigned)RadioConfig::chargingDetectPin);
        } else {
            pinMode(RadioConfig::chargingDetectPin, INPUT_PULLDOWN);
            if (RadioConfig::chargingDetectUseAdc) {
                analogSetPinAttenuation(RadioConfig::chargingDetectPin, ADC_11db);
                uint32_t acc = 0;
                for (uint8_t i = 0; i < 8u; i++) {
                    acc += charging_pin_millivolts();
                    delayMicroseconds(120);
                }
                Serial.printf("[Batt] charge ADC GPIO%u thresh=%umV boot_avg=%umV\n",
                              (unsigned)RadioConfig::chargingDetectPin,
                              (unsigned)RadioConfig::chargingDetectMinMv, (unsigned)(acc / 8u));
            } else {
                Serial.printf("[Batt] charge DIGITAL GPIO%u (pull-down)\n",
                              (unsigned)RadioConfig::chargingDetectPin);
            }
        }
    }
    if (!RadioConfig::batteryMonitorEnable) {
        return;
    }
    pinMode(RadioConfig::batteryAdcPin, INPUT);
    analogSetPinAttenuation(RadioConfig::batteryAdcPin, ADC_11db);
    Serial.printf("[Batt] pack ADC GPIO%u divider=%.2f\n", (unsigned)RadioConfig::batteryAdcPin,
                  (double)RadioConfig::batteryDividerRatio);
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
        if (RadioConfig::chargingDetectEnable && RadioConfig::chargingDebugSerialMs > 0) {
            static uint32_t s_batt_dbg_ms = 0;
            const uint32_t now = millis();
            if ((uint32_t)(now - s_batt_dbg_ms) >= RadioConfig::chargingDebugSerialMs) {
                s_batt_dbg_ms = now;
                const bool chg = battery_is_charging();
                Serial.printf("[Batt] chg=%d pin_mv=%u (gauge off)\n", (int)chg,
                              (unsigned)(RadioConfig::chargingDetectUseAdc ? charging_pin_millivolts() : 0u));
            }
        }
        return false;
    }
    const uint32_t now = millis();
    if (RadioConfig::chargingDetectEnable && RadioConfig::chargingDebugSerialMs > 0) {
        static uint32_t s_batt_dbg_ms = 0;
        if ((uint32_t)(now - s_batt_dbg_ms) >= RadioConfig::chargingDebugSerialMs) {
            s_batt_dbg_ms = now;
            (void)battery_is_charging();
            Serial.printf("[Batt] chg=%d pin_mv=%u thresh=%u pct=%u mv=%u\n", (int)s_charging_cached,
                          (unsigned)(RadioConfig::chargingDetectUseAdc ? charging_pin_millivolts() : 0u),
                          (unsigned)RadioConfig::chargingDetectMinMv, (unsigned)s_percent,
                          (unsigned)s_smooth_mv);
        }
    }
    uint32_t interval = (!radioState.state && RadioConfig::batterySampleIntervalIdleMs > 0)
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

bool battery_low_power_sleep_active() {
    // Без линии зарядки с IP2326 нельзя отличить «идёт зарядка» от «сел АКБ» — не уходим в вечный deep sleep.
    return RadioConfig::batteryShutdownEnable && RadioConfig::batteryMonitorEnable &&
           RadioConfig::chargingDetectEnable;
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
