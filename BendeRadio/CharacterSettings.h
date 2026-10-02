#pragma once
#include <cstdint>

namespace BenderCharacter {
constexpr uint8_t count = 5;
constexpr const char* keys[count] = {"sarcasm", "sociability", "curiosity", "stubbornness", "warmth"};
struct Settings { uint8_t values[count] = {80, 55, 40, 70, 35}; };
constexpr bool valid(const Settings& settings) {
    for (auto value : settings.values) if (value > 100) return false;
    return true;
}
}
