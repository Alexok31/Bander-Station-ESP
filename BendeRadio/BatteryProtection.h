#pragma once
#include <cstdint>

enum class BatteryShutdownReason : uint8_t { None, Low, Critical };

// Independent of displayed SOC and of how often the user requests the gauge.
struct BatteryVoltageGuard {
    bool low = false;
    bool critical = false;
    uint32_t low_since = 0;
    uint32_t critical_since = 0;

    constexpr BatteryShutdownReason sample(uint32_t now, uint16_t mv, bool valid,
                                           bool charging, uint16_t low_mv,
                                           uint16_t critical_mv, uint16_t hysteresis_mv,
                                           uint32_t low_hold_ms, uint32_t critical_hold_ms,
                                           uint32_t charging_hold_ms) {
        if (!valid) {
            low = critical = false;
            return BatteryShutdownReason::None;
        }
        if (mv >= uint32_t(low_mv) + hysteresis_mv) low = false;
        else if (!low && mv < low_mv) { low = true; low_since = now; }
        if (mv >= uint32_t(critical_mv) + hysteresis_mv) critical = false;
        else if (!critical && mv < critical_mv) { critical = true; critical_since = now; }
        if (critical && uint32_t(now - critical_since) >= critical_hold_ms)
            return BatteryShutdownReason::Critical;
        // A charger indication can grant a bounded recovery interval, never disable protection.
        if (low && uint32_t(now - low_since) >= (charging ? charging_hold_ms : low_hold_ms))
            return BatteryShutdownReason::Low;
        return BatteryShutdownReason::None;
    }
};

constexpr bool battery_sense_candidate(uint16_t mv, uint16_t spread,
                                      uint16_t min_mv, uint16_t max_mv,
                                      uint16_t max_spread) {
    return mv >= min_mv && mv <= max_mv && spread <= max_spread;
}
