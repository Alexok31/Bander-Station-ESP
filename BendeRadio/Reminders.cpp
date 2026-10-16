#include "Reminders.h"

#include <Arduino.h>
#include <Preferences.h>
#include <cstring>
#include <ctime>

namespace Reminders {
namespace {
constexpr uint32_t recordMagic = 0x524D4431u;
constexpr time_t firstValidTime = 1700000000;
struct DailyRecord {
    uint32_t magic = recordMagic;
    uint32_t id = 0;
    uint16_t minuteOfDay = 0;
    int32_t lastDay = 0;
    char text[textBytes] = {};
};
struct Slot {
    uint32_t id = 0;
    uint32_t deadline = 0;
    uint16_t minuteOfDay = 0;
    int32_t lastDay = 0;
    bool daily = false;
    char text[textBytes] = {};
};
Slot slots[capacity];
uint32_t nextRelativeId = 0x80000000u;
uint32_t nextDailyId = 0;
portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
SemaphoreHandle_t storageLock = nullptr;

struct StorageGuard {
    bool acquired = false;
    StorageGuard() : acquired(storageLock && xSemaphoreTake(storageLock, pdMS_TO_TICKS(5000)) == pdTRUE) {}
    ~StorageGuard() { if (acquired) xSemaphoreGive(storageLock); }
};

void keyFor(size_t index, char (&key)[8]) {
    snprintf(key, sizeof(key), "d%u", unsigned(index));
}

bool localClock(int32_t& day, uint32_t& secondsToday) {
    const time_t now = time(nullptr);
    tm local{};
    if (now < firstValidTime || !localtime_r(&now, &local)) return false;
    day = (local.tm_year + 1900) * 1000 + local.tm_yday;
    secondsToday = uint32_t(local.tm_hour) * 3600u + uint32_t(local.tm_min) * 60u + local.tm_sec;
    return true;
}

bool writeDaily(size_t index, const Slot& slot) {
    DailyRecord record;
    record.id = slot.id;
    record.minuteOfDay = slot.minuteOfDay;
    record.lastDay = slot.lastDay;
    memcpy(record.text, slot.text, sizeof(record.text));
    char key[8];
    keyFor(index, key);
    Preferences prefs;
    if (!prefs.begin("reminders", false)) return false;
    const bool ok = prefs.putBytes(key, &record, sizeof(record)) == sizeof(record);
    prefs.end();
    return ok;
}

void copyItem(Item& out, const Slot& slot, uint32_t nowMs,
              bool hasClock, int32_t day, uint32_t secondsToday) {
    out.id = slot.id;
    out.daily = slot.daily;
    out.minuteOfDay = slot.minuteOfDay;
    out.today = false;
    if (slot.daily) {
        out.due = hasClock && slot.lastDay != day && secondsToday >= uint32_t(slot.minuteOfDay) * 60u;
        out.today = hasClock && slot.lastDay != day;
        if (hasClock) {
            const int32_t left = int32_t(slot.minuteOfDay) * 60 - int32_t(secondsToday);
            out.remainingSeconds = out.due ? 0 : left > 0 ? uint32_t(left) : uint32_t(86400 + left);
        } else {
            out.remainingSeconds = 0;
        }
    } else {
        const int32_t left = int32_t(slot.deadline - nowMs);
        out.due = left <= 0;
        out.remainingSeconds = left > 0 ? (uint32_t(left) + 999u) / 1000u : 0;
        out.today = hasClock && out.remainingSeconds <= 86400u - secondsToday;
    }
    memcpy(out.text, slot.text, sizeof(out.text));
}
}

void begin() {
    storageLock = xSemaphoreCreateMutex();
    // Kyiv: UTC+2 in winter, UTC+3 in summer. SNTP syncs after Wi-Fi connects.
    configTzTime("EET-2EEST,M3.5.0/3,M10.5.0/4", "pool.ntp.org", "time.google.com");
    Preferences prefs;
    if (!prefs.begin("reminders", true)) return;
    for (size_t i = 0; i < capacity; ++i) {
        char key[8];
        keyFor(i, key);
        if (prefs.getBytesLength(key) != sizeof(DailyRecord)) continue;
        DailyRecord record;
        if (prefs.getBytes(key, &record, sizeof(record)) != sizeof(record) ||
            record.magic != recordMagic || !record.id || record.id >= 0x80000000u ||
            record.minuteOfDay >= 1440 ||
            !record.text[0] || !memchr(record.text, 0, sizeof(record.text))) continue;
        Slot& slot = slots[i];
        slot.id = record.id;
        if (record.id > nextDailyId) nextDailyId = record.id;
        slot.daily = true;
        slot.minuteOfDay = record.minuteOfDay;
        slot.lastDay = record.lastDay;
        memcpy(slot.text, record.text, sizeof(slot.text));
    }
    prefs.end();
}

bool clockReady() { return time(nullptr) >= firstValidTime; }

bool add(uint16_t minutes, const char* text, uint32_t* id) {
    if (!minutes || minutes > maxMinutes) return false;
    return addSeconds(uint32_t(minutes) * 60u, text, id);
}

bool addSeconds(uint32_t seconds, const char* text, uint32_t* id) {
    if (seconds < 5 || seconds > 86400u || !text) return false;
    const size_t length = strlen(text);
    if (!length || length >= textBytes) return false;
    const uint32_t deadline = millis() + seconds * 1000u;
    portENTER_CRITICAL(&lock);
    for (auto& slot : slots) {
        if (slot.id) continue;
        if (++nextRelativeId < 0x80000000u) nextRelativeId = 0x80000001u;
        slot = Slot{};
        slot.deadline = deadline;
        memcpy(slot.text, text, length + 1);
        slot.id = nextRelativeId;
        if (id) *id = slot.id;
        portEXIT_CRITICAL(&lock);
        return true;
    }
    portEXIT_CRITICAL(&lock);
    return false;
}

bool addDaily(uint8_t hour, uint8_t minute, const char* text, uint32_t* id) {
    if (hour > 23 || minute > 59 || !text) return false;
    const size_t length = strlen(text);
    if (!length || length >= textBytes) return false;
    StorageGuard guard;
    if (!guard.acquired) return false;
    int32_t day = 0;
    uint32_t secondsToday = 0;
    const bool hasClock = localClock(day, secondsToday);
    portENTER_CRITICAL(&lock);
    for (size_t i = 0; i < capacity; ++i) {
        if (slots[i].id) continue;
        Slot& slot = slots[i];
        slot = Slot{};
        if (++nextDailyId >= 0x80000000u) nextDailyId = 1;
        slot.id = nextDailyId;
        slot.daily = true;
        slot.minuteOfDay = uint16_t(hour) * 60u + minute;
        if (hasClock && secondsToday >= uint32_t(slot.minuteOfDay) * 60u) slot.lastDay = day;
        memcpy(slot.text, text, length + 1);
        const Slot saved = slot;
        portEXIT_CRITICAL(&lock);
        if (!writeDaily(i, saved)) {
            portENTER_CRITICAL(&lock);
            if (slot.id == saved.id && slot.daily) slot = Slot{};
            portEXIT_CRITICAL(&lock);
            return false;
        }
        if (id) *id = saved.id;
        return true;
    }
    portEXIT_CRITICAL(&lock);
    return false;
}

bool cancel(uint32_t id) {
    if (!id) return false;
    StorageGuard guard;
    if (!guard.acquired) return false;
    portENTER_CRITICAL(&lock);
    for (size_t i = 0; i < capacity; ++i) {
        Slot& slot = slots[i];
        if (slot.id != id) continue;
        const bool daily = slot.daily;
        slot = Slot{};
        portEXIT_CRITICAL(&lock);
        if (daily) {
            char key[8];
            keyFor(i, key);
            Preferences prefs;
            if (!prefs.begin("reminders", false)) return false;
            const bool ok = prefs.remove(key);
            prefs.end();
            return ok;
        }
        return true;
    }
    portEXIT_CRITICAL(&lock);
    return false;
}

bool complete(uint32_t id) {
    StorageGuard guard;
    if (!guard.acquired) return false;
    int32_t day = 0;
    uint32_t secondsToday = 0;
    const bool hasClock = localClock(day, secondsToday);
    portENTER_CRITICAL(&lock);
    for (size_t i = 0; i < capacity; ++i) {
        Slot& slot = slots[i];
        if (slot.id != id) continue;
        if (!slot.daily) {
            slot = Slot{};
            portEXIT_CRITICAL(&lock);
            return true;
        }
        if (!hasClock) { portEXIT_CRITICAL(&lock); return false; }
        slot.lastDay = day;
        const Slot saved = slot;
        portEXIT_CRITICAL(&lock);
        return writeDaily(i, saved);
    }
    portEXIT_CRITICAL(&lock);
    return false;
}

size_t list(Item* out, size_t count) {
    if (!out || !count) return 0;
    int32_t day = 0;
    uint32_t secondsToday = 0;
    const bool hasClock = localClock(day, secondsToday);
    const uint32_t now = millis();
    size_t found = 0;
    portENTER_CRITICAL(&lock);
    for (const auto& slot : slots) {
        if (slot.id && found < count) copyItem(out[found++], slot, now, hasClock, day, secondsToday);
    }
    portEXIT_CRITICAL(&lock);
    return found;
}

bool active() {
    portENTER_CRITICAL(&lock);
    for (const auto& slot : slots) {
        if (slot.id) { portEXIT_CRITICAL(&lock); return true; }
    }
    portEXIT_CRITICAL(&lock);
    return false;
}

bool countdown(uint32_t& seconds) {
    const uint32_t now = millis();
    bool found = false;
    uint32_t nearest = 86400u;
    portENTER_CRITICAL(&lock);
    for (const auto& slot : slots) {
        if (!slot.id || slot.daily) continue;
        const int32_t left = int32_t(slot.deadline - now);
        const uint32_t remaining = left > 0 ? (uint32_t(left) + 999u) / 1000u : 0;
        if (!found || remaining < nearest) nearest = remaining;
        found = true;
    }
    portEXIT_CRITICAL(&lock);
    seconds = found ? nearest : 0;
    return found;
}

bool peekDue(Item& out) {
    int32_t day = 0;
    uint32_t secondsToday = 0;
    const bool hasClock = localClock(day, secondsToday);
    const uint32_t now = millis();
    portENTER_CRITICAL(&lock);
    for (const auto& slot : slots) {
        if (!slot.id) continue;
        const bool due = slot.daily ? hasClock && slot.lastDay != day &&
                          secondsToday >= uint32_t(slot.minuteOfDay) * 60u
                          : int32_t(slot.deadline - now) <= 0;
        if (!due) continue;
        copyItem(out, slot, now, hasClock, day, secondsToday);
        portEXIT_CRITICAL(&lock);
        return true;
    }
    portEXIT_CRITICAL(&lock);
    return false;
}

bool hasDue() { Item due; return peekDue(due); }
}
