#pragma once

#include <cstdint>
#include "BenderLife.h"

// MPU-6050 на I2C: тряска колонки → событие для UI.
void mpu6050_init();
bool mpu6050_ok();
// Опрос по интервалу. true — зафиксирована тряска (с учётом cooldown).
bool mpu6050_poll_shake();
// Call after poll_shake; consumes a sustained-motion edge from that same sample.
BenderEvent mpu6050_motion_event();
