#include "battery_matrix.h"

#include <cstring>

#include "RadioConfig.h"

// Фіксований кадр (специфікація 8×8).
static const uint8_t k_battery_frame[8] = {
    0x00,  // ........
    0x18,  // ...##...
    0x3C,  // ..####..
    0x24,  // ..#..#..
    0x24,
    0x24,
    0x24,
    0x3C,  // ..####..
};

// Порядок заливки змійкою (8 півкроків): ряд 6 знизу L→R, далі R→L, …
static const uint8_t k_snake_row[8] = {6u, 6u, 5u, 5u, 4u, 4u, 3u, 3u};
static const uint8_t k_snake_col[8] = {3u, 4u, 4u, 3u, 3u, 4u, 4u, 3u};

static void compose_inner(uint8_t inner_steps_lit, uint8_t out[8]) {
    (void)memcpy(out, k_battery_frame, 8u);
    const uint8_t n = (inner_steps_lit > 8u) ? 8u : inner_steps_lit;
    for (uint8_t h = 0; h < n; h++) {
        const uint8_t r = k_snake_row[h];
        const uint8_t x = k_snake_col[h];
        out[r] |= (uint8_t)(1u << (7u - x));
    }
}

static void maybe_invert_y(uint8_t rows[8]) {
    if (!RadioConfig::batteryMatrixInvertY) {
        return;
    }
    for (uint8_t a = 0, b = 7u; a < b; a++, b--) {
        const uint8_t t = rows[a];
        rows[a] = rows[b];
        rows[b] = t;
    }
}

uint8_t battery_matrix_inner_steps_from_pct(uint8_t pct) {
    const uint8_t p = (pct > 99u) ? 99u : pct;
    if (p > RadioConfig::batteryMatrixFullMinPct) {
        return 8u;
    }
    return (uint8_t)(((uint16_t)p * 7u) / (uint16_t)RadioConfig::batteryMatrixFullMinPct);
}

void battery_matrix_rows_from_percent(uint8_t pct, uint8_t rows[8]) {
    compose_inner(battery_matrix_inner_steps_from_pct(pct), rows);
    maybe_invert_y(rows);
}

void battery_matrix_rows_charging(uint8_t pct, uint32_t frame, uint8_t rows[8]) {
    const uint8_t base = battery_matrix_inner_steps_from_pct(pct);
    uint8_t inner = base;
    if (base < 8u) {
        const uint32_t span = (uint32_t)(9u - base);
        inner = (uint8_t)(base + (frame % span));
    }
    compose_inner(inner, rows);
    maybe_invert_y(rows);
}
