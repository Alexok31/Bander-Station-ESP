#pragma once

#include <stdint.h>

// Повний заряд (усі 8 кроків заливки) лише при pct > цього значення.
uint8_t battery_matrix_inner_steps_from_pct(uint8_t pct);

// Статичний кадр 8×8 (біт 7 = лівий стовпчик x=0).
void battery_matrix_rows_from_percent(uint8_t pct, uint8_t rows[8]);

// Зарядка: база з pct, у «порожній» зоні циклічно додається заливка знизу вгору (змійка).
void battery_matrix_rows_charging(uint8_t pct, uint32_t frame, uint8_t rows[8]);

// Додаткові імена з ТЗ (виклик тих самих функцій).
static inline void drawBattery(uint8_t percent, uint8_t rows[8]) {
    battery_matrix_rows_from_percent(percent, rows);
}
static inline void drawCharging(uint8_t percent, uint32_t frame, uint8_t rows[8]) {
    battery_matrix_rows_charging(percent, frame, rows);
}
