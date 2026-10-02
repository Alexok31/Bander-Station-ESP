#include "../BendeRadio/WebUiSaveScope.h"
constexpr bool independent_sections() {
    constexpr const char* names[] = {"radio", "wifi", "behavior", "display", "ai", "character"};
    for (unsigned i = 0; i < 6; ++i) {
        const WebUiSaveScope s(names[i]);
        if (!s.valid || s.legacy || s.radio != (i == 0) || s.wifi != (i == 1) ||
            s.behavior != (i == 2) || s.display != (i == 3) || s.ai != (i == 4) || s.character != (i == 5)) return false;
    }
    return !WebUiSaveScope("radio-extra").valid;
}
static_assert(independent_sections(), "Saving one section must not overwrite other settings");
constexpr WebUiSaveScope legacy("");
static_assert(legacy.valid && legacy.legacy && legacy.radio && legacy.wifi && legacy.behavior && legacy.display && legacy.ai && !legacy.character,
              "Cached old full forms remain supported");
