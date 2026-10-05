#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace WakeSample {
enum State : uint8_t { Idle, Queued, Recording, Ready, Failed };
constexpr uint32_t rate = 24000, durationMs = 4000, queueTimeoutMs = 15000, captureTimeoutMs = 6000;
constexpr uint32_t warmupMs = 250;
// Dataset capture and local wake detector: remove DC BEFORE PCM16 saturation. Keep
// floating-point history unclipped so a large input step cannot latch the filter.
// alpha = exp(-2*pi*60/24000). Speech and a fixed gain remain reproducible.
class MicFilter {
    float previous = 0, output = 0;
    bool seeded = false;
public:
    constexpr int16_t process(float input) {
        if (!seeded) { previous = input; seeded = true; return 0; }
        output = 0.98441476f * (output + input - previous);
        previous = input;
        return output > 32767.f ? 32767 : output < -32767.f ? -32767 : int16_t(output);
    }
};
constexpr size_t headerBytes = 44, pcmBytes = rate * 2 * durationMs / 1000;
// Append new labels to preserve existing numeric IDs (negative remains 2).
constexpr const char* labels[] = {"hey_bender", "privet_bender", "negative", "bender"};
constexpr size_t labelCount = sizeof(labels) / sizeof(labels[0]);
constexpr const char* splits[] = {"train", "test"};
inline int labelIndex(const char* text) {
    if (text) for (size_t i = 0; i < labelCount; ++i) if (!strcmp(text, labels[i])) return int(i);
    return -1;
}
inline int splitIndex(const char* text) {
    if (text) for (int i = 0; i < 2; ++i) if (!strcmp(text, splits[i])) return i;
    return -1;
}
constexpr bool active(State state) { return state == Queued || state == Recording; }
constexpr bool parseId(const char* text, uint32_t& id) {
    if (!text || !*text) return false;
    uint64_t n = 0;
    for (; *text; ++text) {
        if (*text < '0' || *text > '9') return false;
        n = n * 10 + unsigned(*text - '0');
        if (n > UINT32_MAX) return false;
    }
    id = uint32_t(n);
    return id != 0;
}
constexpr void put16(uint8_t* p, uint16_t n) { p[0] = n; p[1] = n >> 8; }
constexpr void put32(uint8_t* p, uint32_t n) { put16(p, n); put16(p + 2, n >> 16); }
constexpr void putText(uint8_t* p, const char* text, size_t n) { for (size_t i = 0; i < n; ++i) p[i] = text[i]; }
constexpr void wavHeader(uint8_t* p) {
    putText(p, "RIFF", 4); put32(p + 4, pcmBytes + 36); putText(p + 8, "WAVEfmt ", 8);
    put32(p + 16, 16); put16(p + 20, 1); put16(p + 22, 1);
    put32(p + 24, rate); put32(p + 28, rate * 2); put16(p + 32, 2); put16(p + 34, 16);
    putText(p + 36, "data", 4); put32(p + 40, pcmBytes);
}
struct Status { State state; uint32_t id, elapsedMs; uint8_t label, split; };
constexpr size_t poolCapacity = 30, memoryReserve = 2 * 1024 * 1024;
// Caller serializes all access. Memory belongs to the caller; remove returns it
// for freeing. Incomplete records occupy a slot but never appear in downloads.
class Pool {
public:
    struct Entry { uint32_t id = 0; uint8_t label = 0, split = 0; uint8_t* wav = nullptr; bool ready = false; };
private:
    Entry entries[poolCapacity] = {};
public:
    constexpr size_t count() const { size_t n = 0; for (const auto& e : entries) if (e.wav) ++n; return n; }
    constexpr Entry* find(uint32_t id) { for (auto& e : entries) if (e.wav && e.id == id) return &e; return nullptr; }
    constexpr bool add(uint32_t id, uint8_t label, uint8_t split, uint8_t* wav) {
        if (!id || !wav || label >= labelCount || split >= 2 || find(id)) return false;
        for (auto& e : entries) if (!e.wav) { e = {id, label, split, wav, false}; return true; }
        return false;
    }
    constexpr uint8_t* remove(uint32_t id) {
        auto* e = find(id); if (!e) return nullptr;
        auto* wav = e->wav; *e = {}; return wav;
    }
    constexpr size_t list(Status* out, size_t capacity) const {
        size_t n = 0;
        for (const auto& e : entries) if (e.wav && e.ready && n < capacity)
            out[n++] = {Ready, e.id, durationMs, e.label, e.split};
        return n;
    }
};
}
