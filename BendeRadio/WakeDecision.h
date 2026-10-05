#pragma once
#include <cstdint>

namespace WakeDecision {
// The board measured 130-155 ms per former 100 ms decision. Keep all DSP
// frames, but run the frozen model every 200 ms instead of overrunning PCM.
constexpr uint32_t featureStepMs = 100;
constexpr uint32_t intervalMs = 200;
// Two scores 200 ms apart span the same 200 ms as three scores 100 ms apart.
// Keeping three here would require 400 ms and reject short utterances.
constexpr uint32_t confirmations = 2;
constexpr uint32_t refractoryMs = 2000;
constexpr float threshold = .25f;
static_assert(intervalMs % featureStepMs == 0, "Decisions must align to feature snapshots");

class Cadence {
public:
    void reset() { steps_ = 0; }
    bool advance() {
        if (++steps_ < intervalMs / featureStepMs) return false;
        steps_ = 0; return true;
    }
private: uint32_t steps_ = 0;
};

class Trigger {
public:
    void reset() { hits_ = cooldown_ = 0; }
    bool step(float score) {
        if (cooldown_) { --cooldown_; hits_ = 0; return false; }
        hits_ = score >= threshold ? hits_ + 1 : 0;
        if (hits_ < confirmations) return false;
        hits_ = 0;
        cooldown_ = (refractoryMs + intervalMs - 1) / intervalMs;
        return true;
    }
private: uint32_t hits_ = 0, cooldown_ = 0;
};
}
