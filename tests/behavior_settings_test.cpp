#include "../BendeRadio/BehaviorSettings.h"
#include "../BendeRadio/BenderLife.h"
using namespace BenderBehavior;

static_assert(validShake(shakeDefault) && validShake(shakeMin) && validShake(shakeMax));
static_assert(!validShake(0) && !validShake(11001) && !validShake(65535));
static_assert(validCalm(0) && validCalm(2) && validCalm(5) && validCalm(10) && validCalm(15));
static_assert(!validCalm(1) && !validCalm(30) && !validCalm(65535));

constexpr bool strict_form_numbers() {
    uint16_t value = 42;
    if (parseUnsigned("", value) || parseUnsigned("-1", value) ||
        parseUnsigned("5min", value) || parseUnsigned("never", value) ||
        parseUnsigned("4294967296", value) || value != 42) return false;
    return parseUnsigned("0", value) && value == 0 &&
           parseUnsigned("11000", value) && value == 11000;
}
static_assert(strict_form_numbers(), "Bad form values must not disable automatic sleep");

constexpr bool configured_idle_timeouts() {
    for (auto minutes : calmOptions) {
        const uint32_t timeout = uint32_t(minutes) * 60000u;
        if (!minutes) {
            if (!faceAwake(3600000u, 0, timeout)) return false;
        } else {
            if (!faceAwake(timeout - 1, 0, timeout) || faceAwake(timeout, 0, timeout)) return false;
            // A shake refreshes the activity timestamp, starting a full new interval.
            if (!faceAwake(timeout, timeout, timeout)) return false;
        }
    }
    return faceAwake(499, UINT32_MAX - 499, 1000) && !faceAwake(500, UINT32_MAX - 499, 1000);
}
static_assert(configured_idle_timeouts(), "Selected timeout, never mode and millis rollover");

constexpr bool configured_shake_threshold() {
    BenderShake normal, lessSensitive;
    normal.sample(12000, 1000, 11000, 1500);
    lessSensitive.sample(12000, 1000, 14000, 1500);
    return normal.sample(12000, 1040, 11000, 1500) &&
           !lessSensitive.sample(12000, 1040, 14000, 1500);
}
static_assert(configured_shake_threshold(), "Increasing the selected threshold rejects weaker shakes");
