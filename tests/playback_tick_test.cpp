#include "../BendeRadio/PlaybackCompletion.h"
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#define F(x) x
#define ALOGLN(x) ((void)0)

// Exercise the actual production tick, with controlled PCM/network timing.
static bool s_owns_spk, s_taking, pttHeld, s_ptt_armed, speaking;
static bool responsePending, waitingACK, s_need_speaker, s_dbg_play;
static bool respPlaybackPending, s_giveback, s_ai_awake;
static uint8_t s_demo;
static std::atomic<unsigned> s_wake_voice{0};
static std::atomic<bool> s_dialogue_ending{false};
static std::atomic<bool> s_wake_prompt_pending{false};
static bool s_voice_pending, sessionReady, followup;
namespace WakeRuntime {bool followupEnabled(){return followup;}}
static std::atomic<bool> s_pcm_in_flight{false};
static std::atomic<unsigned> s_preview_state{0};
static uint32_t s_last_pcm_ms, s_tts_out_ms, s_ai_last_live_ms, nowMs;
static size_t queued;
static unsigned releases;
static int convState, mux;
constexpr int ST_IDLE=0, ST_RECORDING=1, ST_WAIT_RESP=2;
void portENTER_CRITICAL(int*){}
void portEXIT_CRITICAL(int*){}
uint32_t millis(){return nowMs;}
size_t rbUsed(){return queued;}
void requestGiveback(){s_giveback=true;}
bool bender_ai_busy(){return s_owns_spk || convState!=ST_IDLE || pttHeld;}
bool takeSpeaker(){s_owns_spk=true;return true;}
void forceRecover(const char*,bool){}
bool applyVoiceCmdState(){return false;}
void releaseSpeakerFromLoop(){++releases;s_owns_spk=false;}
void radio_voice_after_speaker(){}
void bender_ai_sleep(){s_ai_awake=false;}
#include "../artifacts/wake-runtime/playback_tick_under_test.h"

int main() {
    // Worker is already unprimed and will never request giveback. The production
    // loop must release ownership itself, for every subsequent interaction.
    for (unsigned turn=1;turn<=3;++turn) {
        s_owns_spk=true;s_giveback=false;convState=ST_WAIT_RESP;responsePending=true;
        nowMs=turn*10000;s_last_pcm_ms=s_tts_out_ms=nowMs;queued=0;
        nowMs+=450;bender_ai_tick();assert(s_owns_spk);
        convState=ST_IDLE;responsePending=false;respPlaybackPending=true;
        bender_ai_tick();assert(s_owns_spk);
        nowMs+=350;bender_ai_tick();assert(!s_owns_spk&&!s_giveback&&!respPlaybackPending);
        assert(releases==turn);
    }
    s_owns_spk=true;s_giveback=true;s_pcm_in_flight=true;nowMs+=1000;
    bender_ai_tick();assert(s_owns_spk); // queued block is still being handed to I2S
    s_pcm_in_flight=false;s_tts_out_ms=nowMs;
    bender_ai_tick();assert(s_owns_spk);
    nowMs+=350;bender_ai_tick();assert(!s_owns_spk);
    // The completed microphone demo uses the same speaker, and must still exit.
    s_owns_spk=true;s_demo=3;nowMs+=1000;bender_ai_tick();assert(!s_owns_spk&&s_demo==0);
    // Keep ownership only for a live follow-up session. An offline server or a
    // queued radio command must release it rather than wedging the device.
    s_owns_spk=true;s_wake_voice=3;followup=sessionReady=true;
    bender_ai_tick();assert(s_owns_spk);
    sessionReady=false;bender_ai_tick();assert(!s_owns_spk);
    s_owns_spk=true;sessionReady=true;s_voice_pending=true;
    bender_ai_tick();assert(!s_owns_spk);
    s_voice_pending=false;s_owns_spk=true;followup=false;
    bender_ai_tick();assert(!s_owns_spk);
    // Goodbye bypasses follow-up only after the final PCM and DMA tail drain.
    s_owns_spk=s_ai_awake=followup=sessionReady=true;s_wake_voice=3;
    s_dialogue_ending=true;responsePending=true;convState=ST_WAIT_RESP;
    queued=4800;nowMs+=1000;
    bender_ai_tick();assert(s_owns_spk&&s_ai_awake&&s_dialogue_ending);
    responsePending=false;convState=ST_IDLE;queued=0;s_pcm_in_flight=true;
    bender_ai_tick();assert(s_owns_spk&&s_dialogue_ending);
    s_pcm_in_flight=false;s_tts_out_ms=s_last_pcm_ms=nowMs;
    nowMs+=799;bender_ai_tick();assert(s_owns_spk&&s_dialogue_ending);
    ++nowMs;bender_ai_tick();
    assert(!s_owns_spk&&!s_ai_awake&&!s_dialogue_ending&&s_wake_voice==0);
    // A fresh wake conversation still offers follow-up (the preference persists).
    s_owns_spk=s_ai_awake=true;s_wake_voice=3;
    bender_ai_tick();assert(s_owns_spk&&followup);
    // Manual PTT goodbye also sleeps immediately after playback.
    s_wake_voice=0;s_dialogue_ending=true;nowMs+=1000;
    bender_ai_tick();assert(!s_ai_awake&&!s_dialogue_ending&&!s_owns_spk);
    // A wake-only reply must keep I2S for the local prompt even with follow-up off.
    s_owns_spk=true;s_wake_voice=3;followup=false;s_wake_prompt_pending=true;
    bender_ai_tick();assert(s_owns_spk);
    s_wake_voice=1;s_wake_prompt_pending=false;
    s_giveback=true; // A late speaker-task request cannot interrupt the handoff.
    bender_ai_tick();assert(s_owns_spk); // transition to prompt/calibration is not idle playback
    puts("Production AI tick: three completed turns, in-flight PCM, DMA tail, demo completion passed");
}
