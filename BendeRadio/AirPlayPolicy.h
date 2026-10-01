#pragma once
#include <cstdint>

constexpr bool airplay_session_expired(uint32_t now, uint32_t lastPacket,
                                      bool session, bool paused, bool peerAlive, bool wifiOnline) {
    if (!peerAlive || !wifiOnline) return true;
    // Keep a deliberate pause, but recover a silently abandoned live stream or SETUP.
    return session && !paused && uint32_t(now - lastPacket) >= 15000u;
}

struct AirPlayPrebuffer {
    bool armed = false;
    constexpr void reset() { armed = false; }
    constexpr bool ready(uint32_t available, uint32_t required) {
        if (available == 0) armed = false;
        if (!armed && available >= required) armed = true;
        return armed;
    }
};

struct AirPlayRtpOrder {
    bool seen = false;
    uint32_t last = 0;
    constexpr void reset(bool hasBoundary = false, uint32_t boundary = 0) {
        seen = hasBoundary;
        last = boundary;
    }
    constexpr bool accepts(uint32_t timestamp) const {
        const uint32_t delta = timestamp - last;
        return !seen || (delta != 0 && delta < 0x80000000u);
    }
    constexpr void commit(uint32_t timestamp) { seen = true; last = timestamp; }
};

constexpr bool airplay_accept_volume(float db, bool encoderGuard, float encoderDb) {
    // Mute always wins, even during the short encoder echo-suppression window.
    if (db <= -144.0f) return true;
    const float difference = db - encoderDb;
    return !encoderGuard || (difference >= -1.5f && difference <= 1.5f);
}
