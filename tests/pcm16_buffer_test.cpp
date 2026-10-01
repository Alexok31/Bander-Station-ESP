#include "../BendeRadio/Pcm16.h"
#include <cstdint>

// Reproduce overrun with the same byte-ring/free-space arithmetic as firmware.
// The tiny ring forces repeated fills, drops and wraps without needing a board.
constexpr bool simulate_overflow(bool whole_samples) {
    constexpr size_t capacity = 32;
    uint8_t ring[capacity] = {};
    uint16_t expected[512] = {};
    size_t head = 0, tail = 0, written = 0, read = 0;
    for (size_t packet = 0; packet < 100; ++packet) {
        const size_t free = (tail + capacity - head - 1) % capacity;
        const size_t available = free < 16 ? free : 16;
        const size_t take = whole_samples ? pcm16_whole_bytes(available) : available;
        // Each packet contains eight distinct PCM16 sample values.
        for (size_t i = 0; i < take; ++i) {
            const uint16_t sample = uint16_t(0x1200 + packet * 8 + i / 2);
            ring[(head + i) % capacity] = uint8_t(i % 2 ? sample >> 8 : sample);
            if (i % 2) expected[written++] = sample;
        }
        head = (head + take) % capacity;
        if (head % 2 || tail % 2) return false;
        const size_t used = (head + capacity - tail) % capacity;
        const size_t consume = pcm16_whole_bytes(used < 6 ? used : 6);
        for (size_t i = 0; i < consume; i += 2) {
            const uint16_t got = uint16_t(ring[(tail + i) % capacity]) |
                                 uint16_t(ring[(tail + i + 1) % capacity]) << 8;
            if (got != expected[read++]) return false;
        }
        tail = (tail + consume) % capacity;
    }
    return read > 250;
}

static_assert(!simulate_overflow(false), "Old byte writes reproduce lost sample alignment");
static_assert(simulate_overflow(true), "Overflow/drop/wrap must preserve PCM sample values");
static_assert(pcm16_whole_bytes(524287) == 524286, "Full 512-KiB ring must leave a whole sample slot");
static_assert(pcm16_whole_bytes(2) == 2, "Final one-sample tail must drain");
static_assert(pcm16_whole_bytes(1) == 0, "Never read/write half a sample");
static_assert(pcm16_whole_bytes(0) == 0, "Reset ring must not manufacture stale audio");
