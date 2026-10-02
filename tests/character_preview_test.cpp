#include "../BendeRadio/CharacterPreview.h"
using namespace BenderCharacter;
static_assert(validQuestion(defaultQuestion));
static_assert(!validQuestion(nullptr) && !validQuestion("") && !validQuestion(" \t\n"));
static_assert(!validQuestion("hello\x01world"));
static_assert(validQuestion("Привіт! Як справи?"));
constexpr bool limits() {
    char ascii[202] = {};
    for (unsigned i = 0; i < 200; ++i) ascii[i] = 'x';
    if (!validQuestion(ascii)) return false;
    ascii[200] = 'x';
    if (validQuestion(ascii)) return false;
    char unicode[805] = {};
    for (unsigned i = 0; i < 200; ++i) {
        unicode[i*4] = '\xf0'; unicode[i*4+1] = '\x9f';
        unicode[i*4+2] = '\x99'; unicode[i*4+3] = '\x82';
    }
    if (!validQuestion(unicode)) return false;
    unicode[800] = 'x';
    return !validQuestion(unicode);
}
static_assert(limits(), "Do not truncate Unicode questions or overflow preview queue");
