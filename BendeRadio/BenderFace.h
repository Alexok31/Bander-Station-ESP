#pragma once

#include <cstdint>

// Presentation only: these states never take ownership of audio or the microphone.
enum class BenderFaceState : uint8_t { Idle, Listening, Thinking, Speaking, Error };

struct BenderFaceInput {
    bool error;
    bool recording;
    bool recordingReleased;
    bool outputRecent;
    bool waiting;
};

constexpr BenderFaceState bender_face_resolve(BenderFaceInput in) {
    return in.error ? BenderFaceState::Error
         : in.recording ? (in.recordingReleased ? BenderFaceState::Thinking
                                               : BenderFaceState::Listening)
         : in.outputRecent ? BenderFaceState::Speaking
         : in.waiting ? BenderFaceState::Thinking
                      : BenderFaceState::Idle;
}

// Unsigned subtraction also covers millis() rollover.
constexpr bool bender_face_recent(uint32_t now, uint32_t since, uint32_t duration) {
    return since != 0 && uint32_t(now - since) < duration;
}
