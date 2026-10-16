#pragma once

#include <cstdint>

// A sealed 24x8 tank: 72 lit water cells, including at sideways/upside-down angles.
// Pure integer physics; the caller supplies one step roughly every 50 ms.
namespace LiquidMouth {
struct Tank {
    static constexpr uint8_t width = 24, rows = 8, volume = width * 3;
    int16_t gravityX = 0, gravityY = 256;
    int16_t height[width] = {}, velocity[width] = {}, remainder[width] = {};
    uint8_t columns[width] = {};
    uint32_t frame = 0, randomState = 0xB3D3u;

    static constexpr int32_t absValue(int32_t n) { return n < 0 ? -n : n; }
    static constexpr int16_t clamp(int32_t v, int16_t low, int16_t high) {
        return int16_t(v < low ? low : v > high ? high : v);
    }
    static constexpr int16_t ease(int16_t value, int16_t target) {
        return absValue(target - value) < 4 ? target : value + (target - value) / 4;
    }

    constexpr void step(int16_t ax, int16_t az, bool sensorValid, uint8_t sound, uint32_t /*motion*/) {
        ++frame;
        int16_t targetX = 0, targetY = 256;
        if (sensorValid) {
            const int32_t norm = absValue(ax) + absValue(az);
            // When tilted out of the display plane, retain the last usable direction.
            targetX = gravityX; targetY = gravityY;
            if (norm >= 3000) {
                targetX = int32_t(ax) * 256 / norm;
                targetY = int32_t(az) * 256 / norm;
            }
        }
        const int16_t turn = targetX - gravityX;
        gravityX = ease(gravityX, targetX);
        gravityY = ease(gravityY, targetY);
        // Turning excites waves at the edges; impacts do not inject a central spike.
        velocity[0] += clamp(turn / 3, -24, 24);
        velocity[width - 1] -= clamp(turn / 3, -24, 24);
        if (sound > 12 && frame % 3u == 0) {
            randomState = randomState * 1664525u + 1013904223u;
            const uint8_t x = (randomState >> 16) % width;
            velocity[x] -= 8 + uint16_t(sound) * 48u / 255u;
            velocity[(x + 7) % width] += uint16_t(sound) * 24u / 255u;
        }
        for (uint8_t x = 0; x < width; ++x) {
            const int16_t left = height[x ? x - 1 : x];
            const int16_t right = height[x + 1 < width ? x + 1 : x];
            const int16_t restore = height[x] > 0 ? (height[x] + 11) / 12 : (height[x] - 11) / 12;
            velocity[x] = clamp(velocity[x] * 7 / 8 +
                                (left + right - 2 * height[x]) / 4 - restore, -80, 80);
        }
        for (uint8_t x = 0; x < width; ++x) {
            remainder[x] += velocity[x];
            height[x] = clamp(height[x] + remainder[x] / 4, -40, 40);
            remainder[x] %= 4;
        }
    }

    constexpr void rasterize() {
        int16_t potential[width * rows] = {};
        int16_t low = 32767, high = -32767;
        for (uint8_t x = 0; x < width; ++x) {
            columns[x] = 0;
            for (uint8_t y = 0; y < rows; ++y) {
                const int16_t p = gravityX * (2 * int(x) - 23) +
                                  gravityY * (2 * int(y) - 7) - height[x] * 32;
                potential[x * rows + y] = p;
                if (p < low) low = p;
                if (p > high) high = p;
            }
        }
        ++high;
        // Find the water level for a fixed volume; no per-frame allocation or sort.
        while (high - low > 1) {
            const int16_t middle = low + (high - low) / 2;
            uint16_t count = 0;
            for (int16_t p : potential) if (p >= middle) ++count;
            if (count >= volume) low = middle;
            else high = middle;
        }
        uint8_t filled = 0;
        for (uint8_t x = 0; x < width; ++x)
            for (uint8_t y = 0; y < rows; ++y)
                if (potential[x * rows + y] > low) {
                    columns[x] |= uint8_t(1u << y);
                    ++filled;
                }
        for (uint8_t x = 0; x < width && filled < volume; ++x)
            for (uint8_t y = 0; y < rows && filled < volume; ++y)
                if (potential[x * rows + y] == low) {
                    columns[x] |= uint8_t(1u << y);
                    ++filled;
                }
    }
};
}
