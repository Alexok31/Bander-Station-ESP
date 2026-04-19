#pragma once

#include <cstdint>

void battery_init();
void battery_update();
// Немедленный замер (игнор интервала) — вызывать перед показом % по энкодеру и т.п.
void battery_force_sample();
uint8_t battery_percent();
uint16_t battery_millivolts();
// Делитель с линии LED зарядки на chargingDetectPin: true, пока идёт активная зарядка.
bool battery_is_charging();

// 0 веселий (≥batteryMoodCheerfulMinPct), 1 звичайний, 2 сумний (<batteryMoodNormalMinPct).
uint8_t battery_eye_mood();
