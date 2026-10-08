#pragma once
#include <stdint.h>

// MAX7219 has no readback on this wiring. Reassert configuration frequently
// during power settling, then infrequently to recover a later driver reset.
class MatrixRecoverySchedule {
public:
    void start(uint32_t now) { started_ = last_ = now; active_ = true; }
    bool due(uint32_t now) {
        if (!active_) return false;
        const uint32_t interval = uint32_t(now - started_) < 10000u ? 500u : 15000u;
        if (uint32_t(now - last_) < interval) return false;
        last_ = now;
        return true;
    }
private:
    uint32_t started_ = 0, last_ = 0;
    bool active_ = false;
};

template<class WriteRegister, class RestoreBrightness, class RestoreFrame>
void matrix_restore_registers(WriteRegister write, RestoreBrightness brightness, RestoreFrame frame) {
    write(0x0f, 0x00);  // Display test off.
    write(0x09, 0x00);  // Raw pixels, no digit decoding.
    write(0x0b, 0x07);  // Scan all eight rows.
    brightness();      // Preserve the user's per-module settings.
    frame();           // Restore RAM before waking a driver that lost power.
    write(0x0c, 0x01);  // Normal operation; never intentionally blank a live display.
}
