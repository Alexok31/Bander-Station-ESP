#include "../BendeRadio/CharacterSettings.h"
using namespace BenderCharacter;
constexpr uint8_t oldProfile[] = {3, 7, 11, 91, 100};
constexpr auto migrated = fromStored(oldProfile, sizeof(oldProfile));
static_assert(migrated.values[0] == 3 && migrated.values[4] == 100);
static_assert(migrated.values[5] == 45 && migrated.values[6] == 35);
constexpr uint8_t fullProfile[] = {3, 7, 11, 91, 100, 100, 0};
constexpr auto restored = fromStored(fullProfile, sizeof(fullProfile));
static_assert(restored.values[5] == 100 && restored.values[6] == 0);
constexpr uint8_t corrupt[] = {3, 7, 11, 91, 101, 100, 0};
static_assert(fromStored(corrupt, sizeof(corrupt)).values[0] == Settings{}.values[0]);
static_assert(fromStored(fullProfile, 6).values[0] == Settings{}.values[0]);
static_assert(fromStored(nullptr, 7).values[0] == Settings{}.values[0]);
