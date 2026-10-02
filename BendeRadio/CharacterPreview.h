#pragma once
#include <cstddef>

namespace BenderCharacter {
constexpr size_t questionMaxChars = 200;
constexpr size_t questionMaxBytes = questionMaxChars * 4;
constexpr const char* defaultQuestion = "Бендере, як проведемо цей вечір?";

// UTF-8 transport size and character budget; JSON decoding validates Unicode on the server.
constexpr bool validQuestion(const char* text) {
    if (!text) return false;
    size_t bytes = 0, chars = 0;
    bool content = false;
    for (; *text; ++text) {
        const auto c = static_cast<unsigned char>(*text);
        if (++bytes > questionMaxBytes) return false;
        if ((c & 0xc0) != 0x80 && ++chars > questionMaxChars) return false;
        if (c < 32 && c != '\n' && c != '\r' && c != '\t') return false;
        if (c == 127) return false;
        if (c > 32) content = true;
    }
    return content;
}
}
