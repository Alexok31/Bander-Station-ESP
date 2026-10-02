#pragma once

#include <cstdint>
#include "BatteryProtection.h"

void battery_init();
// true, если в этом вызове выполнен новый замер (по интервалу).
bool battery_update();
// Немедленный замер (игнор интервала) — вызывать перед показом % по энкодеру и т.п.
void battery_force_sample();
// true после первого реального ADC-замера (до этого s_percent=0 — не «пустая батарея»).
bool battery_gauge_ready();
uint8_t battery_percent();
uint16_t battery_millivolts();
// Последнее усреднение ADC без медленного фильтра индикатора.
uint16_t battery_protection_millivolts();
BatteryShutdownReason battery_shutdown_reason();
// Делитель с линии LED зарядки на chargingDetectPin: true, пока идёт активная зарядка.
bool battery_is_charging();

// 0 веселий (≥batteryMoodCheerfulMinPct), 1 звичайний, 2 сумний (<batteryMoodNormalMinPct).
uint8_t battery_eye_mood();

// true — делитель на GPIO1 похож на живую 2S (не «обрыв»).
bool battery_sense_present();
// true — защита по свежему напряжению активна (не зависит от детектора зарядки).
bool battery_low_power_sleep_active();
