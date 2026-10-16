#include "../BendeRadio/LiquidMouth.h"

// constexpr tests are executed by the ESP32 compiler, without flashing a test sketch.
using LiquidMouth::Tank;
constexpr int countWater(const Tank& tank) {
    int count = 0;
    for (uint8_t col : tank.columns)
        for (int bit = 0; bit < 8; ++bit) count += (col >> bit) & 1;
    return count;
}
constexpr bool flat(const Tank& tank) {
    for (uint8_t col : tank.columns) if (col != 0xe0) return false;
    return true;
}
constexpr bool rest() {
    Tank t;
    t.step(0, 16000, true, 0, 0);
    t.rasterize();
    return countWater(t) == 72 && flat(t);
}
constexpr bool rotations() {
    Tank t;
    constexpr int16_t directions[][2] = {{16000, 16000}, {16000, 0},
        {0, -16000}, {-16000, 0}, {0, 16000}};
    for (const auto& d : directions) {
        for (int i = 0; i < 60; ++i) t.step(d[0], d[1], true, 0, 0);
        t.rasterize();
        if (countWater(t) != 72) return false;
        if (d[0] > 0 && t.columns[23] == 0) return false;
        if (d[0] < 0 && t.columns[0] == 0) return false;
        if (d[1] < 0 && (t.columns[12] & 1) == 0) return false;
    }
    return flat(t);
}
constexpr bool musicAndSettling() {
    Tank t;
    bool moved = false;
    for (int i = 0; i < 30; ++i) {
        t.step(0, 16000, true, 255, 0);
        t.rasterize();
        if (countWater(t) != 72) return false;
        moved |= !flat(t);
    }
    for (int i = 0; i < 250; ++i) t.step(0, 16000, true, 0, 0);
    t.rasterize();
    return moved && flat(t);
}
constexpr bool stressAndMissingSensor() {
    Tank t;
    for (int i = 0; i < 300; ++i) {
        t.step(i % 2 ? -32768 : 32767, 0, true, 255, 196605);
        for (int x = 0; x < 24; ++x)
            if (Tank::absValue(t.height[x]) > 40 || Tank::absValue(t.velocity[x]) > 80) return false;
    }
    t.rasterize();
    if (countWater(t) != 72) return false;
    for (int i = 0; i < 300; ++i) t.step(0, 0, false, 0, 0);
    t.rasterize();
    return flat(t);
}
static_assert(rest(), "Idle must fill exactly the bottom three rows");
constexpr bool impactWithoutTilt() {
    Tank t;
    for (int i = 0; i < 30; ++i) {
        t.step(0, 16000, true, 0, 196605);
        t.rasterize();
        if (!flat(t) || countWater(t) != 72) return false;
    }
    return true;
}
static_assert(impactWithoutTilt(), "Impact alone must not create an artificial central splash");
static_assert(rotations(), "Water must move with gravity and conserve volume");
static_assert(musicAndSettling(), "Music must animate water and silence must settle it");
static_assert(stressAndMissingSensor(), "Shakes must stay bounded; absent sensor must recover");
int main() { return 0; }
