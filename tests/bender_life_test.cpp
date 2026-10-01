#include "../BendeRadio/BenderLife.h"
using E = BenderEvent;

static_assert(bender_event_face(E::Shake) == BenderFaceState::Annoyed);
static_assert(bender_event_face(E::Charging) == BenderFaceState::Pleased);
static_assert(bender_event_face(E::NetworkLost) == BenderFaceState::Curious);
static_assert(!bender_face_show_error(true, true, false));
static_assert(bender_face_show_error(true, false, false));
static_assert(bender_face_show_error(true, true, true));
static_assert(bender_event_radio_allows_voice(E::Shake, false));
static_assert(bender_event_radio_allows_voice(E::Charging, false));
static_assert(!bender_event_radio_allows_voice(E::Carried, false));
static_assert(bender_event_radio_allows_voice(E::Carried, true));

constexpr bool less_sensitive_shake() {
    BenderShake s;
    if (s.sample(6500, 1000, 8000, 1500) || s.sample(6500, 1040, 8000, 1500)) return false;
    if (s.sample(9000, 1080, 8000, 1500) || s.sample(0, 1400, 8000, 1500)) return false;
    if (s.sample(9000, 1500, 8000, 1500) || !s.sample(10000, 1540, 8000, 1500)) return false;
    if (s.sample(10000, 1580, 8000, 1500) || s.sample(10000, 1620, 8000, 1500)) return false;
    return !s.sample(9000, 3100, 8000, 1500) && s.sample(9000, 3140, 8000, 1500);
}
static_assert(less_sensitive_shake(), "Reject moderate motion, isolated hits and repeated shakes during cooldown");

constexpr bool shake_rollover() {
    BenderShake s;
    return !s.sample(9000, UINT32_MAX - 30, 8000, 1500) &&
        s.sample(9000, 20, 8000, 1500) && !s.sample(9000, 60, 8000, 1500);
}
static_assert(shake_rollover(), "Shake confirmation and cooldown survive millis rollover");

constexpr bool rejected_voice_does_not_start_cooldown() {
    BenderLife life;
    return life.allowVoice(E::Shake, 30000, false) &&
        life.allowVoice(E::Charging, 31000) && !life.allowVoice(E::Shake, 32000, false);
}
static_assert(rejected_voice_does_not_start_cooldown());

constexpr bool debounce_and_rollover() {
    BenderStableBool b;
    if (b.update(false, UINT32_MAX - 2000, 3000)) return false;
    if (b.update(true, UINT32_MAX - 1000, 3000)) return false;
    if (b.update(true, 500, 3000)) return false;
    return b.update(true, 2500, 3000) && b.value;
}
static_assert(debounce_and_rollover(), "Debounce must handle millis rollover");

constexpr bool battery_hysteresis() {
    BenderLife life;
    life.update(1000, true, 50, false, true, true, true, 0, false);
    if (life.update(2000, true, 15, false, true, true, true, 0, false) != E::None) return false;
    if (life.update(5000, true, 15, false, true, true, true, 0, false) != E::BatteryLow) return false;
    for (uint32_t now = 6000; now < 20000; now += 1000)
        if (life.update(now, true, 17, false, true, true, true, 0, false) != E::None) return false;
    life.update(21000, true, 17, true, true, true, true, 0, false);
    return life.update(24000, true, 17, true, true, true, true, 0, false) == E::Charging;
}
static_assert(battery_hysteresis(), "Low charge must not chatter at the threshold");

constexpr bool network_and_station() {
    BenderLife life;
    if (life.update(1000, false, 0, false, true, true, true, 0, true) != E::None) return false;
    life.update(2000, false, 0, false, false, true, true, 0, true);
    if (life.update(5000, false, 0, false, false, true, true, 0, true) != E::NetworkLost) return false;
    life.update(6000, false, 0, false, true, true, true, 0, true);
    if (life.update(9000, false, 0, false, true, true, true, 0, true) != E::NetworkBack) return false;
    life.update(10000, false, 0, false, true, true, true, 1, true);
    if (life.update(11000, false, 0, false, true, true, true, 1, true) != E::None) return false;
    return life.update(13000, false, 0, false, true, true, true, 1, true) == E::FavoriteStation;
}
static_assert(network_and_station(), "Transitions, not every poll, produce events");

constexpr bool limits() {
    BenderLife life;
    if (life.allowVoice(E::Shake, 1000)) return false;
    if (!life.allowVoice(E::Shake, 30000)) return false;
    if (life.allowVoice(E::Charging, 31000)) return false;
    if (life.allowVoice(E::Shake, 150000)) return false;
    if (!life.allowVoice(E::Charging, 150000)) return false;
    if (life.allowVoice(E::NetworkLost, 999999)) return false;
    return life.allowVoice(E::Shake, 400000);
}
static_assert(limits(), "Global and per-event voice cooldowns");

constexpr bool motion() {
    BenderMotion m;
    // One bump followed by quiet must never look like carrying.
    m.sample(10000, 40);
    for (uint32_t t = 80; t < 4000; t += 40)
        if (m.sample(0, t) != E::None) return false;
    bool carried = false, down = false;
    for (uint32_t t = 4000; t < 9000; t += 40)
        if (m.sample(2500, t) == E::Carried) carried = true;
    for (uint32_t t = 9000; t < 15000; t += 40)
        if (m.sample(0, t) == E::SetDown) down = true;
    return carried && down && !m.moving;
}
static_assert(motion(), "Sustained movement and set-down require time, not a single bump");

constexpr bool shaking_is_not_carrying() {
    BenderMotion m;
    for (uint32_t t = 40; t < 6000; t += 40)
        if (m.sample(12000, t) == E::Carried) return false;
    return !m.moving;
}
static_assert(shaking_is_not_carrying(), "Strong shaking is not a calm walk");
