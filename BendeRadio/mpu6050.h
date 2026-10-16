#pragma once

#include <cstdint>
#include "BenderLife.h"

// MPU-6050 на I2C: тряска колонки → событие для UI.
void mpu6050_init();
bool mpu6050_ok();
// Опрос по интервалу. true — зафиксирована тряска (с учётом cooldown).
bool mpu6050_poll_shake();
struct MpuAcceleration {
    int16_t x = 0, y = 0, z = 0;
    uint32_t change = 0; // sum of per-axis changes since the preceding sample
    bool valid = false;
};
// Last sample from poll_shake(); does not perform another I2C transaction.
MpuAcceleration mpu6050_acceleration();
// Call after poll_shake; consumes a sustained-motion edge from that same sample.
BenderEvent mpu6050_motion_event();
