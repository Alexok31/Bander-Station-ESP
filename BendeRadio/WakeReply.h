#pragma once
#include "CharacterSettings.h"

namespace WakeReply {
enum Bank : uint8_t { Neutral, Warm, Sarcastic, Rough, Curious, Profane };

constexpr Bank bankFor(const BenderCharacter::Settings& traits, bool followup) {
    // Follow-up belongs to the same conversation: avoid "you again" every turn.
    if (followup) return Neutral;
    const auto& v = traits.values;
    if (v[6] >= 85) return Profane;
    Bank bank = Neutral;
    uint8_t strongest = 54;
    if (v[0] > strongest) { strongest = v[0]; bank = Sarcastic; }
    if (v[2] > strongest) { strongest = v[2]; bank = Curious; }
    if (v[5] > strongest) { strongest = v[5]; bank = Rough; }
    if (v[4] >= 55 && v[4] >= strongest) bank = Warm;
    return bank;
}

constexpr uint8_t choose(const BenderCharacter::Settings& traits, bool followup,
                         uint32_t random, uint8_t previous) {
    const uint8_t first = uint8_t(bankFor(traits, followup)) * 2;
    uint8_t chosen = first + (random & 1u);
    if (chosen == previous) chosen = first + ((chosen - first) ^ 1u);
    return chosen;
}
}
