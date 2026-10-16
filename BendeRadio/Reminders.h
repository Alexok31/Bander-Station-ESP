#pragma once

#include <cstddef>
#include <cstdint>

namespace Reminders {
constexpr size_t capacity = 24;
constexpr size_t textBytes = 161;
constexpr uint16_t maxMinutes = 24 * 60;

struct Item {
    uint32_t id = 0;
    uint32_t remainingSeconds = 0;
    bool due = false;
    bool daily = false;
    bool today = false;
    uint16_t minuteOfDay = 0;
    char text[textBytes] = {};
};

// Daily plans live in NVS. Relative timers remain in RAM.
void begin();
bool clockReady();
bool add(uint16_t minutes, const char* text, uint32_t* id = nullptr);
bool addSeconds(uint32_t seconds, const char* text, uint32_t* id = nullptr);
bool addDaily(uint8_t hour, uint8_t minute, const char* text, uint32_t* id = nullptr);
bool cancel(uint32_t id);
bool complete(uint32_t id);
size_t list(Item* out, size_t count);
bool active();
bool hasDue();
bool peekDue(Item& out);
// Nearest relative timer only; daily plans do not occupy the face all day.
bool countdown(uint32_t& seconds);
}
