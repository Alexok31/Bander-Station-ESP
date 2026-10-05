#pragma once
#include <cstddef>
#include <cstdint>

namespace BenderPlayback {
// Completion must not depend on the speaker task's prebuffer/primed state:
// a temporary underrun can unprime it before response.done arrives.
struct State {
    bool ownsSpeaker, taking, idle, speaking, waiting, debug, inFlight;
    size_t queuedBytes;
    uint32_t now, lastInputMs, lastOutputMs;
};
constexpr bool readyToRelease(const State& s) {
    return s.ownsSpeaker && !s.taking && s.idle && !s.speaking && !s.waiting &&
        !s.debug && !s.inFlight && s.queuedBytes == 0 &&
        (!s.lastInputMs || uint32_t(s.now-s.lastInputMs) >= 800u) &&
        (!s.lastOutputMs || uint32_t(s.now-s.lastOutputMs) >= 350u);
}
}
