#pragma once

constexpr bool webUiScopeEquals(const char* name, const char* expected) {
    if (!name) return false;
    while (*name && *name == *expected) { ++name; ++expected; }
    return *name == *expected;
}

struct WebUiSaveScope {
    bool valid = false, legacy = false;
    bool radio = false, wifi = false, behavior = false, display = false, ai = false, character = false;
    constexpr explicit WebUiSaveScope(const char* name) {
        legacy = !name || !*name;
        radio = legacy || webUiScopeEquals(name, "radio");
        wifi = legacy || webUiScopeEquals(name, "wifi");
        behavior = legacy || webUiScopeEquals(name, "behavior");
        display = legacy || webUiScopeEquals(name, "display");
        ai = legacy || webUiScopeEquals(name, "ai");
        character = webUiScopeEquals(name, "character"); // Old forms have no personality fields.
        valid = radio || wifi || behavior || display || ai || character;
    }
};
