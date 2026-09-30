#pragma once

#include <cstdint>

// MPU-6050 на I2C: тряска колонки → событие для UI.
void mpu6050_init();
bool mpu6050_ok();
// Опрос по интервалу. true — зафиксирована тряска (с учётом cooldown).
bool mpu6050_poll_shake();
