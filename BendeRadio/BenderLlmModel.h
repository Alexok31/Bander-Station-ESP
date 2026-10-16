#pragma once

#include <stdint.h>
#include <string.h>

namespace BenderLlmModel {
enum : uint8_t { Auto = 0, Grok43 = 1, Sonnet55 = 2, Haiku55 = 3, Luna = 4 };

constexpr const char* id(uint8_t value) {
    switch (value) {
        case Grok43: return "grok-4.3";
        case Sonnet55: return "claude-sonnet-5-5";
        case Haiku55: return "claude-haiku-5-5";
        case Luna: return "gpt-6-luna";
        default: return "auto";
    }
}

inline bool parse(const char* text, uint8_t& value) {
    if (!text) return false;
    for (uint8_t candidate = Auto; candidate <= Luna; ++candidate) {
        if (strcmp(text, id(candidate)) == 0) {
            value = candidate;
            return true;
        }
    }
    return false;
}
}
