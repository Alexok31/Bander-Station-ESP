#pragma once
#include <cstdint>
#include <cstddef>

namespace BenderCharacter {
constexpr uint8_t count = 7;
constexpr const char* keys[count] = {"sarcasm", "sociability", "curiosity", "stubbornness", "warmth", "roughness", "profanity"};
struct Settings { uint8_t values[count] = {80, 55, 40, 70, 35, 45, 35}; };
constexpr bool valid(const Settings& settings) {
    for (auto value : settings.values) if (value > 100) return false;
    return true;
}
constexpr Settings fromStored(const uint8_t* bytes, size_t size) {
    Settings settings;
    if (!bytes || (size != 5 && size != count)) return settings;
    for (size_t i = 0; i < size; ++i) {
        if (bytes[i] > 100) return Settings{};
        settings.values[i] = bytes[i];
    }
    return settings;
}
}
