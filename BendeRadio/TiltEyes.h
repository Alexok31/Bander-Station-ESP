#pragma once
#include <cstdint>

namespace TiltEyes {
struct Gaze {
    int32_t filtered = 0;
    uint32_t lastMs = 0, holdUntil = 0, reference = 0x40000000u;
    bool initialized = false, squint = false;
    static constexpr int32_t abs(int32_t n) { return n < 0 ? -n : n; }
    constexpr void step(uint32_t now, int32_t x, int32_t z, bool valid,
                        uint32_t change, uint32_t ref) {
        uint32_t dt = now - lastMs;
        if (!initialized || dt > 1000 || reference != ref) {
            filtered = 0; squint = false; holdUntil = now;
            dt = 150; initialized = true; reference = ref;
        }
        lastMs = now;
        if (dt > 200) dt = 200;
        const int16_t sine = int16_t(ref & 0xffffu), cosine = int16_t(ref >> 16);
        const int32_t horizontal = int32_t((int64_t(x) * cosine - int64_t(z) * sine) / 16384);
        const int32_t vertical = int32_t((int64_t(x) * sine + int64_t(z) * cosine) / 16384);
        const int32_t norm = abs(horizontal) + abs(vertical);
        int32_t target = 0;
        if (valid && change < 8000u && norm >= 3000) {
            target = horizontal * 8192 / norm;
            if (abs(target) < 800) target = 0;
            else holdUntil = now + 250u;
        }
        // Keep the glance briefly after returning upright, then ease back.
        if (!target && int32_t(now - holdUntil) < 0) return;
        filtered += (target - filtered) * int32_t(dt) / int32_t((target ? 220 : 400) + dt);
        if (!target && abs(filtered) < 30) filtered = 0;
        if (abs(filtered) > 4000) squint = true;
        else if (abs(filtered) < 3000) squint = false;
    }
    constexpr int8_t offset() const {
        const int32_t amount = abs(filtered);
        const int8_t pixels = amount >= 3400 ? 2 : amount >= 1200 ? 1 : 0;
        return filtered < 0 ? -pixels : pixels;
    }
    constexpr bool active() const { return abs(filtered) >= 500 || squint; }
};
}
