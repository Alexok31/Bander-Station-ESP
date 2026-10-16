#pragma once

#include <cstdint>

namespace SoloPong {
struct Game {
    constexpr explicit Game(uint8_t w) : width(w) { reset(); }
    uint8_t width;
    int8_t x = 0, y = 4, vx = 1, vy = 1;
    int8_t left = 2;
    uint16_t score = 0;
    bool waiting = true;

    constexpr void reset() {
        x = width / 2; y = 4; vx = 1; vy = 1;
        left = 2; score = 0; waiting = true;
    }
    constexpr void serve(uint32_t seed) {
        reset();
        vx = 1; // Start toward the wall to give the player time to prepare.
        vy = (seed & 2u) ? 1 : -1;
        waiting = false;
    }
    constexpr uint16_t intervalMs() const {
        const uint32_t speedup = uint32_t(score) * 4u;
        return speedup >= 90u ? 90u : uint16_t(180u - speedup);
    }
    constexpr void nudge(int8_t dir) {
        if (!waiting && dir) {
            const int8_t next = left + (dir > 0 ? 1 : -1);
            if (next >= 0 && next <= 5) left = next;
        }
    }
    constexpr bool step(uint32_t) {
        if (waiting) return false;
        y += vy;
        if (y < 0) { y = 0; vy = 1; }
        else if (y > 7) { y = 7; vy = -1; }
        const int8_t next = x + vx;
        if (next >= width - 1) {
            x = width - 2;
            vx = -1; // The solid wall always returns the ball.
        } else if (next <= 0) {
            if (y < left || y > left + 2) {
                x = 0; // Keep the missed ball at the actual exit for the loss animation.
                waiting = true;
                return true;
            }
            x = 1;
            vx = 1;
            if (score < UINT16_MAX) ++score; // Each player's return adds a point and speed.
            if (y == left) vy = -1;
            else if (y == left + 2) vy = 1;
        } else x = next;
        return false;
    }
};
}

namespace SoloSnake {
struct Game {
    static constexpr uint16_t capacity = 24u * 8u;
    explicit Game(uint8_t w) : width(w) { reset(); }
    uint8_t width;
    uint8_t body[capacity] = {};
    uint16_t length = 3;
    uint8_t direction = 0; // right, down, left, up
    uint8_t food = 0;
    uint16_t points = 0;
    bool waiting = true;
    bool turnQueued = false;
    uint8_t hitCell = 0;
    bool hitSelf = false;

    uint16_t score() const { return points; }
    uint16_t intervalMs() const {
        const uint16_t speedup = points * 6u;
        return speedup >= 140u ? 140u : uint16_t(280u - speedup);
    }
    void reset() {
        const uint8_t start = 4u * width + width / 2u;
        body[0] = start; body[1] = start - 1u; body[2] = start - 2u;
        length = 3; direction = 0; points = 0; waiting = true;
        turnQueued = false; hitCell = start; hitSelf = false;
        food = 2u * width + width / 3u;
    }
    void serve(uint32_t seed) { reset(); spawnFood(seed); waiting = false; }
    void turn(int8_t dir) {
        if (!waiting && dir && !turnQueued) {
            direction = uint8_t((direction + (dir > 0 ? 1 : 3)) & 3u);
            turnQueued = true;
        }
    }
    bool occupied(uint8_t cell, uint16_t count) const {
        for (uint16_t i = 0; i < count; ++i) if (body[i] == cell) return true;
        return false;
    }
    void spawnFood(uint32_t seed) {
        const uint16_t cells = uint16_t(width) * 8u;
        uint16_t cell = seed % cells;
        while (occupied(uint8_t(cell), length)) cell = (cell + 1u) % cells;
        food = uint8_t(cell);
    }
    bool step(uint32_t seed) {
        if (waiting) return false;
        turnQueued = false;
        const uint8_t hx = body[0] % width, hy = body[0] / width;
        // The 24x8 display has no visible wall: crossing an edge wraps around.
        const int16_t nx = (int16_t(hx) + (direction == 0) - (direction == 2) + width) % width;
        const int16_t ny = (int16_t(hy) + (direction == 1) - (direction == 3) + 8) % 8;
        const uint8_t next = uint8_t(ny * width + nx);
        const bool grow = next == food;
        if (occupied(next, grow ? length : length - 1u)) {
            hitCell = next; hitSelf = true; waiting = true; return true;
        }
        if (grow && length < uint16_t(width) * 8u) ++length;
        for (uint16_t i = length - 1u; i > 0; --i) body[i] = body[i - 1u];
        body[0] = next;
        if (grow) {
            ++points;
            if (length == uint16_t(width) * 8u) { waiting = true; return true; }
            spawnFood(seed);
        }
        return false;
    }
};
}

namespace SoloDodge {
struct Game {
    explicit Game(uint8_t w) : width(w) { reset(); }
    uint8_t width;
    uint32_t rows[8] = {};
    uint8_t player = 0;
    uint16_t score = 0;
    uint16_t ticks = 0;
    bool waiting = true;

    void reset() {
        for (auto& row : rows) row = 0;
        player = width / 2u; score = ticks = 0; waiting = true;
    }
    void serve(uint32_t) { reset(); waiting = false; }
    uint16_t intervalMs() const {
        const uint16_t speedup = score * 2u;
        return speedup >= 90u ? 80u : uint16_t(170u - speedup);
    }
    bool collision() const {
        uint32_t ship = uint32_t(1) << player;
        if (player > 0) ship |= uint32_t(1) << (player - 1u);
        if (player + 1u < width) ship |= uint32_t(1) << (player + 1u);
        return (rows[7] & ship) != 0;
    }
    bool nudge(int8_t dir) {
        if (waiting || !dir) return false;
        const int16_t next = int16_t(player) + (dir > 0 ? 1 : -1);
        if (next >= 0 && next < width) player = uint8_t(next);
        if (collision()) { waiting = true; return true; }
        return false;
    }
    bool step(uint32_t seed) {
        if (waiting) return false;
        if (rows[7] && score < UINT16_MAX) ++score;
        for (int i = 7; i > 0; --i) rows[i] = rows[i - 1];
        rows[0] = 0;
        const uint8_t spacing = score >= 20 ? 2u : 3u;
        if (++ticks % spacing == 0) {
            const uint8_t x = seed % (width - 1u);
            rows[0] = uint32_t(3) << x;
        }
        if (collision()) { waiting = true; return true; }
        return false;
    }
};
}
