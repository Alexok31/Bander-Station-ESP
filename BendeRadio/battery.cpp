#include "battery.h"
#include "BatteryGauge.h"
#include "BatteryAdc.h"

#include <Arduino.h>
#include <esp_adc/adc_oneshot.h>
#include <esp_adc/adc_cali_scheme.h>

#include "RadioConfig.h"
#include "core0.h"

extern Data radioState;

static uint32_t s_last_sample_ms;
static uint16_t s_smooth_mv;
static uint16_t s_protection_mv;
static uint16_t s_adc_mv;
static uint16_t s_adc_spread;
static uint16_t s_adc_raw_spread;
static BatteryVoltageGuard s_voltage_guard;
static BatteryShutdownReason s_shutdown_reason = BatteryShutdownReason::None;
static uint8_t s_percent;
static BatteryGauge s_gauge;
static BatteryChargeDebounce s_charge_filter;
static uint32_t s_last_display_sample_ms;
static bool s_gauge_ready;
static bool s_sense_present;  // latched: делитель реально бачили
static uint8_t s_sense_latch_streak;

static uint32_t s_charging_read_ms;
static bool s_charging_cached;

// Own ADC2 explicitly: Arduino's analogReadMilliVolts hides read errors as 0 mV.
// The battery divider stays on Arduino's ADC1; never create a second ADC1 owner.
static adc_oneshot_unit_handle_t s_charge_adc;
static adc_cali_handle_t s_charge_cali;
static adc_channel_t s_charge_channel;
static esp_err_t s_charge_init_error = ESP_ERR_INVALID_STATE;
static esp_err_t s_charge_last_error = ESP_OK;
static uint32_t s_charge_ok_reads, s_charge_error_reads, s_charge_high_reads;
static uint16_t s_charge_last_mv, s_charge_peak_mv, s_charge_peak_raw;

static esp_err_t charging_adc_init() {
    if (s_charge_adc && s_charge_cali) return ESP_OK;
    adc_unit_t unit;
    esp_err_t err = adc_oneshot_io_to_channel(RadioConfig::chargingDetectPin, &unit, &s_charge_channel);
    if (err != ESP_OK) return err;
    if (unit != ADC_UNIT_2) return ESP_ERR_NOT_SUPPORTED;
    adc_oneshot_unit_init_cfg_t config = {};
    config.unit_id = unit;
    err = adc_oneshot_new_unit(&config, &s_charge_adc);
    if (err != ESP_OK) return err;
    adc_oneshot_chan_cfg_t channel = {};
    channel.atten = ADC_ATTEN_DB_12;
    channel.bitwidth = ADC_BITWIDTH_12;
    err = adc_oneshot_config_channel(s_charge_adc, s_charge_channel, &channel);
    if (err == ESP_OK) {
        adc_cali_curve_fitting_config_t calibration = {};
        calibration.unit_id = unit;
        calibration.chan = s_charge_channel;
        calibration.atten = channel.atten;
        calibration.bitwidth = channel.bitwidth;
        err = adc_cali_create_scheme_curve_fitting(&calibration, &s_charge_cali);
    }
    if (err != ESP_OK) {
        adc_oneshot_del_unit(s_charge_adc);
        s_charge_adc = nullptr;
    }
    return err;
}

static_assert(RadioConfig::batterySocFullMv > 8200, "SOC profile full voltage must exceed 8.2 V");

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

static bool charging_pin_millivolts(uint16_t &mv) {
    int raw = 0, voltage = 0;
    esp_err_t err = s_charge_init_error;
    if (err == ESP_OK) err = adc_oneshot_read(s_charge_adc, s_charge_channel, &raw);
    if (err == ESP_OK) {
        if (raw > s_charge_peak_raw) s_charge_peak_raw = raw;
        err = adc_cali_raw_to_voltage(s_charge_cali, raw, &voltage);
    }
    if (err != ESP_OK) {
        ++s_charge_error_reads;
        s_charge_last_error = err;
        return false;
    }
    mv = voltage > 0 ? voltage : 0;
    s_charge_last_mv = mv;
    if (mv > s_charge_peak_mv) s_charge_peak_mv = mv;
    ++s_charge_ok_reads;
    if (mv >= RadioConfig::chargingDetectMinMv) ++s_charge_high_reads;
    return true;
}

static void charging_debug() {
    if (!RadioConfig::chargingDetectUseAdc) return;
    Serial.printf("[Charge] GPIO%u last_mv=%u peak_mv=%u raw_peak=%u ok=%lu errors=%lu high=%lu last_err=%s\n",
        (unsigned)RadioConfig::chargingDetectPin, (unsigned)s_charge_last_mv,
        (unsigned)s_charge_peak_mv, (unsigned)s_charge_peak_raw,
        (unsigned long)s_charge_ok_reads, (unsigned long)s_charge_error_reads,
        (unsigned long)s_charge_high_reads, esp_err_to_name(s_charge_last_error));
    s_charge_ok_reads = s_charge_error_reads = s_charge_high_reads = 0;
    s_charge_peak_mv = s_charge_peak_raw = 0;
    s_charge_last_error = ESP_OK;
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
            if (!charging_pin_millivolts(mv)) {
                // A failed conversion is not a measurement. Try the remaining
                // conversions with the same bounded delay, without blocking Wi-Fi.
                delayMicroseconds(200);
                continue;
            }
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
    s_protection_mv = 0;
    s_adc_mv = s_adc_spread = s_adc_raw_spread = 0;
    s_voltage_guard = {};
    s_shutdown_reason = BatteryShutdownReason::None;
    s_percent = 0;
    s_gauge = {};
    s_charge_filter = {};
    s_last_display_sample_ms = 0;
    s_gauge_ready = false;
    s_sense_present = false;
    s_sense_latch_streak = 0;
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
                s_charge_init_error = charging_adc_init();
                Serial.printf("[Batt] charge ADC2 GPIO%u thresh=%umV init=%s\n",
                              (unsigned)RadioConfig::chargingDetectPin,
                              (unsigned)RadioConfig::chargingDetectMinMv,
                              esp_err_to_name(s_charge_init_error));
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
    const bool chg = battery_is_charging();

    constexpr uint8_t kSamples = 64;
    uint16_t samples[kSamples];
    for (uint8_t i = 0; i < kSamples; i++) {
        samples[i] = (uint16_t)adc_pin_millivolts();
    }
    const auto reading = battery_adc_reading(samples);
    const uint32_t pin_mv = reading.mv;
    const uint16_t pin_spread = reading.spread;
    s_adc_mv = (uint16_t)pin_mv;
    s_adc_spread = pin_spread;
    s_adc_raw_spread = reading.raw_spread;
    const float ratio = RadioConfig::batteryDividerRatio;
    uint32_t pack_mv = (uint32_t)((float)pin_mv * ratio + 0.5f);
    if (pack_mv > 20000u) {
        pack_mv = 20000u;
    }
    s_protection_mv = (uint16_t)pack_mv;

    const uint32_t now = millis();
    const bool display_valid = battery_sense_candidate(s_protection_mv, pin_spread,
        RadioConfig::batterySensePresentMinMv, RadioConfig::batterySensePresentMaxMv,
        RadioConfig::batterySenseStablePinSpreadMv);
    if (display_valid && (!s_gauge_ready || uint32_t(now-s_last_display_sample_ms) >= 1000u)) {
        s_last_display_sample_ms = now;
        s_smooth_mv = s_gauge_ready ? battery_display_filter(s_smooth_mv, s_protection_mv) : s_protection_mv;
        const uint8_t target = battery_voltage_percent(s_smooth_mv, RadioConfig::batterySocFullMv);
        s_percent = s_gauge.sample(now, target, chg);
        s_gauge_ready = true;
    }

    // Обрив GPIO1: шумний/середній АЦП. Реальний 2S у вікні + стабільний pin → latch.
    const bool candidate = battery_sense_candidate(s_protection_mv, pin_spread,
        RadioConfig::batterySensePresentMinMv, RadioConfig::batterySensePresentMaxMv,
        RadioConfig::batterySenseStablePinSpreadMv);
    if (s_sense_present) {
        // Keep confirmation through discharge; a subsequent zero is also a low-voltage fault.
    } else if (candidate) {
        if (++s_sense_latch_streak >= RadioConfig::batterySenseLatchSamples) {
            s_sense_present = true;
            Serial.printf("[Batt] sense OK mv=%u pin_spread=%u — voltage protection armed\n",
                          (unsigned)s_protection_mv, (unsigned)pin_spread);
        }
    } else {
        if (s_sense_latch_streak > 0) {
            s_sense_latch_streak = 0;
        }
        static uint32_t s_sense_warn_ms = 0;
        const uint32_t now = millis();
        if (s_sense_warn_ms == 0 || (uint32_t)(now - s_sense_warn_ms) > 15000u) {
            s_sense_warn_ms = now;
            Serial.printf("[Batt] no pack sense (mv=%u spread=%u) — voltage protection unarmed\n",
                          (unsigned)s_protection_mv, (unsigned)pin_spread);
        }
    }
    // Do not let display smoothing, the SOC table or a CHG pin mask undervoltage.
    const bool valid = battery_low_power_sleep_active() &&
                       s_protection_mv <= RadioConfig::batterySensePresentMaxMv &&
                       pin_spread <= RadioConfig::batterySenseStablePinSpreadMv;
    const auto reason = s_voltage_guard.sample(millis(), s_protection_mv, valid, chg,
        RadioConfig::batteryShutdownBelowMv, RadioConfig::batteryCriticalMv,
        RadioConfig::batteryShutdownHysteresisMv, RadioConfig::batteryShutdownHoldMs,
        RadioConfig::batteryCriticalHoldMs, RadioConfig::batteryChargingRecoveryMs);
    if (s_shutdown_reason == BatteryShutdownReason::None) s_shutdown_reason = reason;
}

void battery_force_sample() {
    if (!RadioConfig::batteryMonitorEnable) {
        return;
    }
    battery_sample_apply();
    // A manual gauge request must not postpone the scheduled protection sample.
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
                              (unsigned)s_charge_last_mv);
                charging_debug();
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
            Serial.printf("[Batt] chg=%d chg_pin_mv=%u thresh=%u pct=%u mv=%u raw_mv=%u sense=%d ready=%d adc_mv=%u spread=%u raw_spread=%u\n", (int)s_charging_cached,
                          (unsigned)s_charge_last_mv,
                          (unsigned)RadioConfig::chargingDetectMinMv, (unsigned)s_percent,
                          (unsigned)s_smooth_mv, (unsigned)s_protection_mv, (int)s_sense_present,
                          (int)s_gauge_ready, (unsigned)s_adc_mv, (unsigned)s_adc_spread,
                          (unsigned)s_adc_raw_spread);
            charging_debug();
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

uint16_t battery_protection_millivolts() { return s_protection_mv; }
BatteryShutdownReason battery_shutdown_reason() { return s_shutdown_reason; }

bool battery_is_charging() {
    if (!RadioConfig::chargingDetectEnable) {
        return false;
    }
    const uint32_t now = millis();
    if (s_charging_read_ms == 0u || (uint32_t)(now - s_charging_read_ms) >= 80u) {
        s_charging_read_ms = now;
        const bool previous = s_charging_cached;
        s_charging_cached = s_charge_filter.sample(now, charging_pin_majority_high());
        if (previous != s_charging_cached) {
            Serial.printf("[Charge] %s\n", s_charging_cached ? "connected" : "signal lost");
        }
    }
    return s_charging_cached;
}

bool battery_sense_present() {
    return RadioConfig::batteryMonitorEnable && s_gauge_ready && s_sense_present;
}

bool battery_low_power_sleep_active() {
    // Charge detection can be absent or faulty; it must not disable voltage protection.
    return RadioConfig::batteryShutdownEnable && RadioConfig::batteryMonitorEnable &&
           battery_sense_present();
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
