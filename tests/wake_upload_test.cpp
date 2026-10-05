#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>

// Link the extracted, unchanged production flushRecording() against controlled
// transport/time boundaries. No device or network required.
static unsigned preN, preHead, sends, recoveries, hangups, polls;
static bool sessionReady, wsReady, pttHeld, s_ptt_armed, voiceEnabled;
static int convState;
constexpr int ST_RECORDING=1;
static std::atomic<unsigned> s_wake_voice{0};
static uint32_t clockMs;
static bool dropOnPoll, disableOnPoll;
namespace WakeRuntime { bool voiceEnabled(){return ::voiceEnabled;} }
uint32_t millis(){return clockMs;}
void vTaskDelay(unsigned){clockMs+=1000;}
void forceRecover(const char*,bool){++recoveries;convState=0;}
void requestHangup(){++hangups;}
void drainPrebufToWs(unsigned){if(sessionReady&&preN){--preN;++sends;}}
struct Client { void poll(){++polls;if(dropOnPoll)sessionReady=wsReady=false;if(disableOnPoll)voiceEnabled=false;} } client;
static Client* wsClient=&client;
#include "../artifacts/wake-runtime/flush_recording_under_test.h"

void reset(unsigned chunks=3) {
    preN=chunks;preHead=7;sends=recoveries=hangups=polls=clockMs=0;
    sessionReady=wsReady=pttHeld=s_ptt_armed=voiceEnabled=true;
    convState=ST_RECORDING;s_wake_voice=2;dropOnPoll=disableOnPoll=false;
}
int main() {
    reset();assert(flushRecording());assert(sends==3&&preN==0&&recoveries==0);
    reset();dropOnPoll=true;assert(!flushRecording());
    assert(sends==1&&preN==0&&recoveries==1&&hangups==1&&!pttHeld&&!s_ptt_armed);
    reset();voiceEnabled=false;assert(!flushRecording());assert(sends==0);
    reset();s_wake_voice=1;voiceEnabled=false;assert(!flushRecording());assert(sends==0);
    reset();disableOnPoll=true;assert(!flushRecording());assert(sends==1);
    reset();s_wake_voice=0;voiceEnabled=false;assert(flushRecording());assert(sends==3); // manual PTT
    reset(100);assert(!flushRecording());assert(clockMs==8000&&sends==8&&preN==0);
    reset();convState=0;assert(!flushRecording());assert(sends==0);
    reset(1);dropOnPoll=true;assert(!flushRecording());assert(sends==1); // no commit after final poll disconnect
    reset();sessionReady=false;assert(!flushRecording());assert(sends==0);
    puts("Recording upload: success, disconnect, disable, manual PTT, timeout, recovery passed");
}
