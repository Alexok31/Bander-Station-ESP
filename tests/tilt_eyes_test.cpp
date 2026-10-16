#include "../BendeRadio/TiltEyes.h"

constexpr bool tiltEyesTest() {
    TiltEyes::Gaze left, right, noise;
    constexpr uint32_t neutral = 0x40000000u;
    for (uint32_t now = 150; now <= 3000; now += 150) {
        left.step(now, -15000, 9000, true, 100, neutral);
        right.step(now, 15000, 9000, true, 100, neutral);
        noise.step(now, (now / 150) % 2 ? 500 : -500, 16384, true, 100, neutral);
        if (left.filtered != -right.filtered || left.offset() != -right.offset()) return false;
        if (right.offset() > 2 || left.offset() < -2 || noise.active()) return false;
    }
    if (left.offset() != -2 || right.offset() != 2 || !right.squint) return false;
    const auto held = right.filtered;
    right.step(3150, 0, 16384, true, 100, neutral);
    if (right.filtered != held) return false;
    for (uint32_t now = 3300; now <= 7500; now += 150)
        right.step(now, 0, 0, false, 0, neutral);
    if (right.active() || right.offset() || right.squint) return false;
    // Recalibration is shared with the wave; diagonal becomes the new neutral.
    const uint32_t diagonal = uint32_t(11585) | (uint32_t(11585) << 16);
    left.step(3150, 10000, 10000, true, 0, diagonal);
    if (left.active() || left.offset()) return false;
    TiltEyes::Gaze impact;
    for (uint32_t now = 150; now <= 3000; now += 150)
        impact.step(now, 30000, 1000, true, 20000, neutral);
    return !impact.active();
}
static_assert(tiltEyesTest(), "Gaze must be symmetric, bounded, calibrated and resistant to noise/impacts");
