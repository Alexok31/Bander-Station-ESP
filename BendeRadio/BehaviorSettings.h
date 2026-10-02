#pragma once
#include <cstdint>

namespace BenderBehavior {
constexpr uint16_t shakeMin = 4000, shakeMax = 24000, shakeStep = 500, shakeDefault = 11000;
constexpr uint8_t calmDefaultMinutes = 5;
constexpr uint8_t calmOptions[] = {2, 5, 10, 15, 0};

constexpr bool validShake(uint16_t value) {
    return value >= shakeMin && value <= shakeMax && value % shakeStep == 0;
}
constexpr bool validCalm(uint16_t value) {
    for (auto option : calmOptions) if (value == option) return true;
    return false;
}
// Strict parsing: malformed text must not become 0 ("never sleep").
constexpr bool parseUnsigned(const char* text, uint16_t& out) {
    if (!text || !*text) return false;
    uint32_t value = 0;
    for (; *text; ++text) {
        if (*text < '0' || *text > '9') return false;
        value = value * 10u + uint32_t(*text - '0');
        if (value > UINT16_MAX) return false;
    }
    out = uint16_t(value);
    return true;
}
constexpr bool faceAwake(uint32_t now, uint32_t lastActivity, uint32_t calmAfterMs) {
    return calmAfterMs == 0 || uint32_t(now - lastActivity) < calmAfterMs;
}
}
