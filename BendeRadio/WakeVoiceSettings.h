#pragma once
#include <cstdint>
namespace WakeVoiceSettings {
constexpr uint8_t defaultFollowupSeconds=8;
constexpr uint8_t maxFollowupSeconds=30;
constexpr bool validFollowup(unsigned seconds){return seconds<=maxFollowupSeconds;}
}
