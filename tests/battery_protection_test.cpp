#include "../BendeRadio/BatteryProtection.h"
using R = BatteryShutdownReason;

constexpr R feed(BatteryVoltageGuard& guard, uint32_t now, uint16_t mv,
                 bool charging = false, bool valid = true) {
    return guard.sample(now, mv, valid, charging, 6200, 5600, 100, 10000, 2000, 30000);
}

constexpr bool low_voltage_timing() {
    BatteryVoltageGuard guard;
    if (feed(guard, 0, 7400) != R::None) return false;
    // Current voltage wins even if the display is still at 7.4 V / 42%.
    if (feed(guard, 1000, 6000) != R::None) return false;
    if (feed(guard, 10999, 6000) != R::None) return false;
    return feed(guard, 11000, 6000) == R::Low;
}
static_assert(low_voltage_timing(), "Sustained low voltage shuts down after 10 seconds");

constexpr bool transient_and_hysteresis() {
    BatteryVoltageGuard guard;
    feed(guard, 0, 5500);
    if (feed(guard, 1000, 6400) != R::None) return false;
    if (feed(guard, 2000, 6400) != R::None) return false;
    feed(guard, 3000, 6100);
    if (feed(guard, 12000, 6250) != R::None) return false;
    if (feed(guard, 13000, 6300) != R::None) return false;
    return feed(guard, 14000, 6100) == R::None;
}
static_assert(transient_and_hysteresis(), "Short bass dips do not trip; recovery clears the timer");

constexpr bool charging_is_bounded() {
    BatteryVoltageGuard guard;
    feed(guard, 0, 5800, true);
    if (feed(guard, 10000, 5800, true) != R::None) return false;
    if (feed(guard, 29999, 5800, true) != R::None) return false;
    if (feed(guard, 30000, 5800, true) != R::Low) return false;
    guard = {};
    feed(guard, 0, 5500, true);
    if (feed(guard, 1999, 5500, true) != R::None) return false;
    return feed(guard, 2000, 5500, true) == R::Critical;
}
static_assert(charging_is_bounded(), "CHG never masks critical voltage or indefinite low voltage");

constexpr bool charger_recovery_and_unplug() {
    BatteryVoltageGuard guard;
    feed(guard, 0, 5800, true);
    if (feed(guard, 29000, 6400, true) != R::None) return false;
    if (feed(guard, 30000, 6400, false) != R::None) return false;
    feed(guard, 40000, 5800, true);
    return feed(guard, 51000, 5800, false) == R::Low;
}
static_assert(charger_recovery_and_unplug());

constexpr bool manual_samples_do_not_accelerate() {
    BatteryVoltageGuard guard;
    for (uint32_t now = 0; now < 10000; now += 100)
        if (feed(guard, now, 6000) != R::None) return false;
    return feed(guard, 10000, 6000) == R::Low;
}
static_assert(manual_samples_do_not_accelerate(), "Hold time is elapsed time, not sample count");

constexpr bool invalid_and_wrap() {
    BatteryVoltageGuard guard;
    feed(guard, 0, 5500);
    if (feed(guard, 1000, 0, false, false) != R::None) return false;
    if (feed(guard, 2000, 5500) != R::None) return false;
    if (feed(guard, 4000, 5500) != R::Critical) return false;
    guard = {};
    feed(guard, UINT32_MAX - 999u, 5500);
    if (feed(guard, 999, 5500) != R::None) return false;
    return feed(guard, 1000, 5500) == R::Critical;
}
static_assert(invalid_and_wrap(), "Bad samples reset confirmation; millis rollover is supported");
static_assert(battery_sense_candidate(5800, 20, 1000, 9200, 100), "Low pack can arm at boot");
static_assert(battery_sense_candidate(5400, 20, 1000, 9200, 100), "Critical pack can arm at boot");
static_assert(!battery_sense_candidate(0, 0, 1000, 9200, 100), "Disconnected zero is not a pack at boot");
static_assert(!battery_sense_candidate(8000, 300, 1000, 9200, 100), "Noisy input cannot arm");
static_assert(!battery_sense_candidate(10000, 20, 1000, 9200, 100), "Out-of-range input cannot arm");
