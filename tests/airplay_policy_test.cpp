#include "../BendeRadio/AirPlayPolicy.h"

constexpr bool pause_and_resume() {
    AirPlayPrebuffer b;
    if (b.ready(89, 90)) return false;
    if (!b.ready(90, 90) || !b.ready(1, 90)) return false;
    b.reset(); // FLUSH/PAUSE must re-arm only after fresh prebuffering.
    if (b.ready(1, 90) || b.ready(89, 90)) return false;
    if (!b.ready(90, 90)) return false;
    if (b.ready(0, 90) || b.ready(1, 90)) return false; // packet starvation
    return b.ready(90, 90);
}
static_assert(pause_and_resume(), "Pause and underrun must wait for a fresh audio buffer");

constexpr bool flush_drops_old_audio() {
    AirPlayRtpOrder order;
    order.reset(true, 10000);
    if (order.accepts(9999) || order.accepts(10000)) return false;
    if (!order.accepts(10352)) return false;
    order.commit(10352);
    if (order.accepts(10352) || order.accepts(10000)) return false;
    order.reset(true, UINT32_MAX - 100);
    if (!order.accepts(251)) return false;
    order.commit(251);
    if (order.accepts(UINT32_MAX - 100)) return false;
    order.reset();
    return order.accepts(0);
}
static_assert(flush_drops_old_audio(), "Late/repeated packets cannot revive a flushed stream; RTP timestamps can wrap");
static_assert(!airplay_session_expired(15999, 1000, true, false, true, true));
static_assert(airplay_session_expired(16000, 1000, true, false, true, true), "Missing RTP times out");
static_assert(!airplay_session_expired(3600000, 1000, true, true, true, true), "A connected pause is not a dropout");
static_assert(airplay_session_expired(2000, 1000, true, true, false, true), "A disconnected paused owner is released");
static_assert(airplay_session_expired(2000, 1000, true, false, true, false), "Wi-Fi loss releases ownership");
static_assert(airplay_session_expired(2000, 1000, false, false, false, true), "An abandoned ANNOUNCE is retired too");
static_assert(!airplay_session_expired(1000, UINT32_MAX - 13000, true, false, true, true));
static_assert(airplay_session_expired(2000, UINT32_MAX - 13000, true, false, true, true), "Timeout handles millis rollover");
static_assert(airplay_accept_volume(-144.f, false, 0.f), "Initial mute must be accepted");
static_assert(airplay_accept_volume(-144.f, true, 0.f), "Mute beats encoder echo suppression");
static_assert(!airplay_accept_volume(-25.f, true, -10.f));
static_assert(airplay_accept_volume(-10.5f, true, -10.f));
static_assert(airplay_accept_volume(-25.f, false, -10.f));
