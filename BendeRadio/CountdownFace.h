#pragma once
#include <cstdint>

namespace CountdownFace {
// A 24x8 mouth: MM:SS below one hour; H HH:MM for longer timers.
constexpr uint8_t digits[10][5] = {
        {7,5,5,5,7}, {2,6,2,2,7}, {7,1,7,4,7}, {7,1,7,1,7}, {5,5,7,1,1},
        {7,4,7,1,7}, {7,4,7,5,7}, {7,1,1,1,1}, {7,5,7,5,7}, {7,5,7,1,7}
};
template<class Dot>
constexpr void draw(uint32_t seconds, Dot dot) {
    const bool hours = seconds >= 3600;
    const uint32_t value = hours ? seconds / 60 : seconds;
    const uint8_t left = uint8_t(value / 60), right = uint8_t(value % 60);
    const uint8_t numbers[] = {uint8_t(left / 10), uint8_t(left % 10),
                               uint8_t(right / 10), uint8_t(right % 10)};
    const uint8_t start = hours ? 7 : 3;
    const uint8_t offsets[] = {0, 4, 9, 13};
    if (hours) {
        for (uint8_t y = 1; y < 6; ++y) { dot(0, y); dot(2, y); }
        dot(1, 3);
    }
    for (uint8_t d = 0; d < 4; ++d)
        for (uint8_t y = 0; y < 5; ++y)
            for (uint8_t x = 0; x < 3; ++x)
                if (digits[numbers[d]][y] & (4 >> x)) dot(start + offsets[d] + x, y + 1);
    dot(start + 7, 2);
    dot(start + 7, 4);
}
}
