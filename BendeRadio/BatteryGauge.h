#pragma once
#include <cstdint>

constexpr uint16_t battery_display_filter(uint16_t previous, uint16_t measured) {
    return (uint32_t(previous)*7u + measured + 4u)/8u;
}

// Approximate voltage profile, not coulomb counting. Final voltage is configurable.
constexpr uint8_t battery_voltage_percent(uint16_t mv, uint16_t full_mv) {
    constexpr uint16_t voltage[] = {6000,6150,6300,6500,6700,6900,7100,7300,7500,7700,7900,8050,8200};
    constexpr uint8_t percent[] = {0,3,7,12,18,25,33,42,50,58,68,76,84};
    if (mv <= voltage[0]) return 0;
    if (mv >= full_mv) return 99; // Mouth has two digits.
    for (unsigned i = 1; i < 13; ++i) {
        if (mv <= voltage[i])
            return percent[i-1] + (uint32_t(mv-voltage[i-1]) * (percent[i]-percent[i-1]) +
                                   (voltage[i]-voltage[i-1])/2) / (voltage[i]-voltage[i-1]);
    }
    return 84 + (uint32_t(mv-8200) * 15 + (full_mv-8200)/2) / (full_mv-8200);
}

struct BatteryChargeDebounce {
    bool charging = false;
    bool seen = false;
    uint32_t last_high = 0;
    constexpr bool sample(uint32_t now, bool input) {
        if (input) {
            // The input already requires 3 high ADC conversions in a burst.
            // Confirm with a second burst, allowing a blinking CHG LED between them.
            if (seen && uint32_t(now-last_high) <= 1500u) charging = true;
            seen = true;
            last_high = now;
        } else if (seen && uint32_t(now-last_high) >= 2500u) {
            charging = false;
            seen = false;
        }
        return charging;
    }
};

struct BatteryGauge {
    bool ready = false;
    bool charging = false;
    uint8_t percent = 0;
    uint32_t changed_at = 0;
    uint32_t stepped_at = 0;
    constexpr uint8_t sample(uint32_t now, uint8_t target, bool charge) {
        if (target > 99) target = 99;
        if (!ready) {
            ready = true; percent = target; charging = charge;
            changed_at = stepped_at = now;
            return percent;
        }
        if (charge != charging) {
            charging = charge; changed_at = stepped_at = now;
        }
        // Plug/unplug voltage transients are not instantaneous changes in stored energy.
        if (uint32_t(now-changed_at) < 30000u) return percent;
        // Allow a decrease even while CHG is active: the load can exceed charging power.
        const uint32_t step_ms = target > percent ? (charging ? 20000u : 60000u) : 10000u;
        if (target == percent) { stepped_at = now; return percent; }
        if (uint32_t(now-stepped_at) >= step_ms) {
            percent = target > percent ? percent+1 : percent-1;
            stepped_at = now; // No catch-up jumps after a gap or repeated manual requests.
        }
        return percent;
    }
};
