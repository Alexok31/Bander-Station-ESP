#include "../BendeRadio/CountdownFace.h"
#include <initializer_list>
constexpr bool countdownFits() {
    // Boundary transitions and each numeral fit without touching the eyes.
    for (uint32_t seconds : {0u, 1u, 12u, 34u, 56u, 59u, 60u, 61u, 78u, 89u,
                             599u, 600u, 3599u, 3600u, 3601u, 86399u, 86400u}) {
        bool pixels[24][8] = {};
        unsigned count = 0;
        bool valid = true;
        CountdownFace::draw(seconds, [&](uint8_t x, uint8_t y) {
            if (x >= 24 || y >= 8) { valid = false; return; }
            if (pixels[x][y]) valid = false;
            pixels[x][y] = true;
            ++count;
        });
        if (!valid || count <= 20 || !pixels[seconds >= 3600 ? 14 : 10][2] ||
            !pixels[seconds >= 3600 ? 14 : 10][4] || pixels[0][1] != (seconds >= 3600)) return false;
    }
    return true;
}
static_assert(countdownFits(), "Countdown must fit the mouth at unit transitions");
