#pragma once
#include <cstdint>
#include "BenderFace.h"

enum class BenderEvent : uint8_t {
    None, BatteryLow, Charging, NetworkLost, NetworkBack, FavoriteStation, Shake, Carried, SetDown
};

constexpr bool bender_event_radio_allows_voice(BenderEvent event, bool radioOn) {
    return radioOn || event == BenderEvent::Shake || event == BenderEvent::Charging;
}

constexpr bool bender_event_allows_motion_wake(BenderEvent event, bool awake, bool wakeOnShake) {
    // Carry/set-down edges from the same movement must not bypass the switch.
    const bool motion = event == BenderEvent::Shake || event == BenderEvent::Carried ||
                        event == BenderEvent::SetDown;
    return !motion || awake || wakeOnShake;
}

constexpr BenderFaceState bender_event_face(BenderEvent event) {
    switch (event) {
        case BenderEvent::Charging:
        case BenderEvent::FavoriteStation:
        case BenderEvent::SetDown: return BenderFaceState::Pleased;
        case BenderEvent::Carried:
        case BenderEvent::NetworkBack:
        case BenderEvent::NetworkLost: return BenderFaceState::Curious;
        case BenderEvent::BatteryLow: return BenderFaceState::Tired;
        case BenderEvent::Shake: return BenderFaceState::Annoyed;
        default: return BenderFaceState::Idle;
    }
}

struct BenderShake {
    uint32_t firstHit = 0, lastShake = 0;
    bool candidate = false, fired = false;
    constexpr bool sample(uint32_t delta, uint32_t now, uint32_t threshold, uint32_t cooldown) {
        if (fired && uint32_t(now - lastShake) < cooldown) return false;
        if (candidate && uint32_t(now - firstHit) > 240) candidate = false;
        if (delta < threshold) return false;
        if (!candidate) { candidate = true; firstHit = now; return false; }
        candidate = false;
        fired = true;
        lastShake = now;
        return true;
    }
};

inline const char* bender_event_name(BenderEvent e) {
    switch (e) {
        case BenderEvent::BatteryLow: return "battery_low";
        case BenderEvent::Charging: return "charging";
        case BenderEvent::NetworkLost: return "network_lost";
        case BenderEvent::NetworkBack: return "network_back";
        case BenderEvent::FavoriteStation: return "favorite_station";
        case BenderEvent::Shake: return "shake";
        case BenderEvent::Carried: return "carried";
        case BenderEvent::SetDown: return "set_down";
        default: return "";
    }
}

// Hardware-free edge/debounce and cooldown logic. Unsigned subtraction handles
// millis() rollover. No startup announcements and no delayed event backlog.
struct BenderStableBool {
    bool initialized = false, value = false, candidate = false;
    uint32_t since = 0;
    constexpr bool update(bool next, uint32_t now, uint32_t delayMs) {
        if (!initialized) { initialized = true; value = candidate = next; since = now; return false; }
        if (next != candidate) { candidate = next; since = now; }
        if (candidate != value && uint32_t(now - since) >= delayMs) { value = candidate; return true; }
        return false;
    }
};

struct BenderLife {
    BenderStableBool charge, network, low;
    bool lostNetwork = false, stationInit = false, radioWasOn = false;
    int station = -1, stationCandidate = -1;
    uint32_t stationSince = 0, lastAnyVoice = 0, lastVoice[9] = {};
    bool voiceSeen[9] = {}, anyVoice = false;

    constexpr BenderEvent update(uint32_t now, bool validBattery, uint8_t pct, bool charging,
                       bool online, bool networkExpected, bool radioOn, int currentStation,
                       bool favorite) {
        BenderEvent out = BenderEvent::None;
        if (validBattery) {
            if (charge.update(charging, now, 3000) && charging) out = BenderEvent::Charging;
            const bool isLow = !charging && pct <= (low.value ? 24 : 15);
            if (low.update(isLow, now, 3000) && low.value) out = BenderEvent::BatteryLow;
        }
        if (network.update(online, now, 3000)) {
            if (!online && networkExpected) { lostNetwork = true; out = BenderEvent::NetworkLost; }
            else if (online && lostNetwork) { lostNetwork = false; out = BenderEvent::NetworkBack; }
        }
        if (!stationInit) {
            stationInit = true; station = stationCandidate = currentStation; radioWasOn = radioOn;
        }
        if (currentStation != stationCandidate || radioOn != radioWasOn) {
            stationCandidate = currentStation; stationSince = now;
            if (radioOn && !radioWasOn) station = -1;
            radioWasOn = radioOn;
        }
        if (station != stationCandidate && uint32_t(now - stationSince) >= 2500) {
            station = stationCandidate;
            if (radioOn && favorite && out == BenderEvent::None) out = BenderEvent::FavoriteStation;
        }
        return out;
    }

    constexpr bool allowVoice(BenderEvent event, uint32_t now, bool commit = true) {
        const auto i = static_cast<uint8_t>(event);
        if (!i || i >= 9 || event == BenderEvent::NetworkLost || now < 30000) return false;
        const uint32_t limits[9] = {0, 1800000, 600000, 0, 900000, 900000, 300000, 600000, 600000};
        if ((anyVoice && uint32_t(now - lastAnyVoice) < 120000) ||
            (voiceSeen[i] && uint32_t(now - lastVoice[i]) < limits[i])) return false;
        if (commit) {
            anyVoice = voiceSeen[i] = true;
            lastAnyVoice = lastVoice[i] = now;
        }
        return true;
    }
};

// Sustained motion is only a "being carried" heuristic, not a position sensor.
// One bump cannot trigger it. Vibration thresholds need verification on hardware.
struct BenderMotion {
    bool initialized = false, moving = false;
    uint32_t windowStart = 0, lastActive = 0;
    uint16_t active = 0, samples = 0;
    uint8_t movingWindows = 0;
    constexpr BenderEvent sample(uint32_t delta, uint32_t now) {
        if (!initialized) { initialized = true; windowStart = lastActive = now; }
        ++samples;
        if (delta > 1400) {
            lastActive = now;
            if (delta < 5500) ++active;
            else movingWindows = 0;  // strong shaking isn't a calm walk
        }
        if (uint32_t(now - windowStart) >= 1000) {
            // At least 8 samples, half with significant acceleration changes.
            if (samples >= 8 && active * 2 >= samples) {
                if (movingWindows < 3) ++movingWindows;
            } else movingWindows = 0;
            windowStart = now; active = samples = 0;
            if (!moving && movingWindows >= 3) { moving = true; return BenderEvent::Carried; }
        }
        if (moving && uint32_t(now - lastActive) >= 5000) {
            moving = false; movingWindows = 0; return BenderEvent::SetDown;
        }
        return BenderEvent::None;
    }
};
