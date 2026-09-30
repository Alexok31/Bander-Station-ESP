#include "../BendeRadio/BenderFace.h"

using Face = BenderFaceState;

// Compile-time regression tests; no ESP32 hardware or Arduino headers required.
static_assert(bender_face_resolve({false, false, false, false, false}) == Face::Idle,
              "Idle/prewarming must not pretend to record");
static_assert(bender_face_resolve({false, true, false, false, false}) == Face::Listening,
              "A held, recording PTT shows listening");
static_assert(bender_face_resolve({false, true, true, false, false}) == Face::Thinking,
              "Releasing PTT shows thinking while the recorded audio is still uploading");
static_assert(bender_face_resolve({false, false, false, false, true}) == Face::Thinking,
              "Queued output is not yet audible speech");
static_assert(bender_face_resolve({false, false, false, true, true}) == Face::Speaking,
              "Actual output takes priority over a pending server response");
static_assert(bender_face_resolve({false, false, false, true, false}) == Face::Speaking,
              "The playback tail survives response.done");
static_assert(bender_face_resolve({false, true, false, true, true}) == Face::Listening,
              "A new recording wins over the previous playback tail");
static_assert(bender_face_resolve({true, true, false, true, true}) == Face::Error,
              "A recording interrupted by a connection failure shows the error");
static_assert(!bender_face_recent(50u, 0u, 300u), "Unset timestamps must be inactive");
static_assert(bender_face_recent(1299u, 1000u, 300u), "Keep speech through short gaps");
static_assert(!bender_face_recent(1300u, 1000u, 300u), "Speech hold expires at its boundary");
static_assert(bender_face_recent(3199u, 1000u, 2200u), "Error remains visible briefly");
static_assert(!bender_face_recent(3200u, 1000u, 2200u), "Error returns to the normal face");
static_assert(bender_face_recent(20u, UINT32_MAX - 20u, 300u), "Handle timer rollover");
static_assert(!bender_face_recent(400u, UINT32_MAX - 20u, 300u), "Expire across rollover");
