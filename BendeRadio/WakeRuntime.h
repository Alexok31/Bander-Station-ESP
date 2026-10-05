#pragma once
#include <cstddef>
#include <cstdint>
namespace WakeRuntime {
// Worker only detects. The existing AI task consumes a fresh hit in voice mode.
struct Status {
    bool enabled, listening, ready;
    uint32_t decisions, detections, dropped, resets, lastHitMs;
    uint32_t processingUs, peakProcessingUs, arenaBytes, stackFreeBytes;
    uint32_t frontendUs, inferenceUs, overruns, queueHighWater;
    float score;
    uint8_t error; // 1 memory/task, 2 model/schema, 3 tensors, 4 invoke, 5 microphone
};
const char* enable(bool on, bool voice = false); // AI startup restores the saved voice preference.
bool enabled();
bool voiceEnabled();
bool followupEnabled();
uint8_t followupSeconds();
void setFollowupSeconds(uint8_t seconds);
bool takeDetection(); // One-shot, rejects hits from an earlier capture epoch.
Status status();
// Only the existing microphone reader calls these. Nonblocking bounded queue.
void listening(bool active);
void submit(const int16_t* pcm, size_t samples);
void gap();
void micError();
}
