#include "mpu6050.h"

#include <Arduino.h>
#include <Wire.h>

#include "RadioConfig.h"
#include "NvsConfig.h"

static bool s_ok = false;
static uint32_t s_last_poll_ms = 0;
static BenderShake s_shake;
static uint16_t s_shake_threshold = RadioConfig::mpu6050ShakeDeltaSum;
static int16_t s_prev_ax = 0;
static int16_t s_prev_ay = 0;
static int16_t s_prev_az = 0;
static bool s_have_prev = false;
static BenderMotion s_motion;
static BenderEvent s_motion_event = BenderEvent::None;

BenderEvent mpu6050_motion_event() {
    const auto event = s_motion_event;
    s_motion_event = BenderEvent::None;
    return event;
}

static bool mpu_write8(uint8_t reg, uint8_t val) {
    Wire.beginTransmission(RadioConfig::mpu6050I2cAddr);
    Wire.write(reg);
    Wire.write(val);
    return Wire.endTransmission() == 0;
}

static bool mpu_read_bytes(uint8_t reg, uint8_t* buf, uint8_t n) {
    Wire.beginTransmission(RadioConfig::mpu6050I2cAddr);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) {
        return false;
    }
    const uint8_t got = Wire.requestFrom((int)RadioConfig::mpu6050I2cAddr, (int)n);
    if (got != n) {
        return false;
    }
    for (uint8_t i = 0; i < n; i++) {
        buf[i] = (uint8_t)Wire.read();
    }
    return true;
}

void mpu6050_init() {
    s_motion = BenderMotion{};
    s_motion_event = BenderEvent::None;
    s_ok = false;
    s_have_prev = false;
    s_last_poll_ms = 0;
    s_shake = BenderShake{};
    s_shake_threshold = nvsLoadShakeThreshold();
    if (!RadioConfig::mpu6050Enable) {
        return;
    }

    Wire.begin(RadioConfig::mpu6050SdaPin, RadioConfig::mpu6050SclPin);
    Wire.setClock(400000);
    delay(50);

    uint8_t who = 0;
    if (!mpu_read_bytes(0x75, &who, 1)) {
        Serial.printf("[MPU] no ACK addr=0x%02X SDA=%u SCL=%u\n",
                      (unsigned)RadioConfig::mpu6050I2cAddr, (unsigned)RadioConfig::mpu6050SdaPin,
                      (unsigned)RadioConfig::mpu6050SclPin);
        return;
    }
    // 0x68 = MPU6050; некоторые клоны отдают 0x70/0x98.
    if (who != 0x68 && who != 0x70 && who != 0x98) {
        Serial.printf("[MPU] unexpected WHO_AM_I=0x%02X\n", (unsigned)who);
    }

    if (!mpu_write8(0x6B, 0x00)) {  // wake
        Serial.println(F("[MPU] PWR_MGMT_1 fail"));
        return;
    }
    delay(10);
    (void)mpu_write8(0x1C, 0x00);  // accel ±2g
    (void)mpu_write8(0x1B, 0x00);  // gyro ±250 dps (не обязателен для тряски)

    s_ok = true;
    Serial.printf("[MPU] OK WHO=0x%02X SDA=%u SCL=%u shakeΔ≥%u\n", (unsigned)who,
                  (unsigned)RadioConfig::mpu6050SdaPin, (unsigned)RadioConfig::mpu6050SclPin,
                  (unsigned)s_shake_threshold);
}

bool mpu6050_ok() {
    return s_ok;
}

bool mpu6050_poll_shake() {
    if (!RadioConfig::mpu6050Enable || !s_ok) {
        return false;
    }
    const uint32_t now = millis();
    if ((uint32_t)(now - s_last_poll_ms) < RadioConfig::mpu6050PollMs) {
        return false;
    }
    s_last_poll_ms = now;

    uint8_t raw[6];
    if (!mpu_read_bytes(0x3B, raw, 6)) {
        return false;
    }
    const int16_t ax = (int16_t)((raw[0] << 8) | raw[1]);
    const int16_t ay = (int16_t)((raw[2] << 8) | raw[3]);
    const int16_t az = (int16_t)((raw[4] << 8) | raw[5]);

    if (!s_have_prev) {
        s_prev_ax = ax;
        s_prev_ay = ay;
        s_prev_az = az;
        s_have_prev = true;
        return false;
    }

    const int32_t dx = (int32_t)ax - (int32_t)s_prev_ax;
    const int32_t dy = (int32_t)ay - (int32_t)s_prev_ay;
    const int32_t dz = (int32_t)az - (int32_t)s_prev_az;
    s_prev_ax = ax;
    s_prev_ay = ay;
    s_prev_az = az;

    const uint32_t sum =
        (uint32_t)((dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy) + (dz < 0 ? -dz : dz));
    const auto motion = s_motion.sample(sum, now);
    if (motion != BenderEvent::None) s_motion_event = motion;
    if (!s_shake.sample(sum, now, s_shake_threshold, RadioConfig::mpu6050ShakeCooldownMs)) {
        return false;
    }
    Serial.printf("[MPU] SHAKE Δ=%lu ax=%d ay=%d az=%d\n", (unsigned long)sum, (int)ax, (int)ay,
                  (int)az);
    return true;
}
