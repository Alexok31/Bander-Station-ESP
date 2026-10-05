#include "../BendeRadio/WakeVoice.h"
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <vector>
#include <cstdio>
constexpr size_t CHUNK=240, CHUNK_BYTES=480;
constexpr uint16_t PREBUF_N=2000;
constexpr uint16_t WAKE_PREROLL_N=320;
constexpr int MIC_GAIN=4;
static uint8_t storage[PREBUF_N*CHUNK_BYTES];
static uint8_t* prebuf=storage;
static uint16_t preN,preHead;
static uint32_t recSamples,recMs,recClip,s_wake_since,s_wake_no_speech_ms;
static int32_t recPeak;
static bool s_wake_followup,s_wake_invocation;
static std::atomic<bool> s_wake_local_capture{false},s_wake_recorded{false},s_wake_prompt_pending{false};
static std::atomic<unsigned> s_wake_warmup{0},s_wake_voice{0};
static WakeVoice::Endpoint s_wake_endpoint;
static WakeVoice::Result s_wake_initial_result;
static unsigned starts;
#define ALOG(...) ((void)0)
uint32_t millis(){return 10000;}
void resetRecStats(){recSamples=recMs=recClip=0;recPeak=0;}
void startPttRecording(){assert(s_wake_voice==2);++starts;}
#include "../artifacts/wake-runtime/wake_initial_under_test.h"

void chunk(int16_t* pcm,int amplitude){for(unsigned i=0;i<CHUNK;++i)pcm[i]=(i&1)?amplitude:-amplitude;}
void seed(unsigned quiet,unsigned speech,unsigned tail){
    preHead=PREBUF_N-80;preN=0;starts=0;
    int16_t pcm[CHUNK];
    for(unsigned i=0;i<quiet+speech+tail;++i){
        chunk(pcm,i>=quiet&&i<quiet+speech?1000:20);
        scaleWakeChunk(reinterpret_cast<uint8_t*>(pcm));
        prePush(reinterpret_cast<uint8_t*>(pcm));
    }
}
WakeVoice::Result live(int amplitude){
    int16_t pcm[CHUNK];chunk(pcm,amplitude);
    return recordWakeInitialChunk(reinterpret_cast<uint8_t*>(pcm));
}
int main(){
    // Detection arrives after the name AND the first words. Preserve the wrapped
    // pre-roll byte-for-byte and count its speech before reading anything else.
    seed(240,80,0);
    const std::vector<uint8_t> before(storage,storage+sizeof(storage));
    beginWakeInitialCapture();
    assert(starts==1&&preN==320&&recMs==3200&&s_wake_invocation&&s_wake_local_capture);
    assert(s_wake_endpoint.heardSpeech()&&s_wake_initial_result==WakeVoice::Continue);
    assert(memcmp(before.data(),storage,sizeof(storage))==0);
    for(unsigned i=0;i<60;++i)assert(live(1000)==WakeVoice::Continue);
    for(unsigned i=0;i<99;++i)assert(live(20)==WakeVoice::Continue);
    assert(live(20)==WakeVoice::Finish);assert(preN==480);
    // A short complete question is already in pre-roll when a late hit arrives.
    seed(210,70,40);beginWakeInitialCapture();
    for(unsigned i=0;i<59;++i)assert(live(20)==WakeVoice::Continue);
    assert(live(20)==WakeVoice::Finish);
    // Calling just the name follows the same path; server decides from ASR text,
    // not guessed timing. No local acknowledgement can interrupt these frames.
    seed(250,40,30);beginWakeInitialCapture();
    for(unsigned i=0;i<69;++i)assert(live(20)==WakeVoice::Continue);
    assert(live(20)==WakeVoice::Finish);
    // Silence-only false detection is cancelled rather than committed.
    seed(320,0,0);beginWakeInitialCapture();
    WakeVoice::Result result=WakeVoice::Continue;
    for(unsigned i=0;i<300&&result==WakeVoice::Continue;++i)result=live(20);
    assert(result==WakeVoice::Cancel);
    // Long speech ends before the FIFO would wrap over the original question.
    seed(240,80,0);beginWakeInitialCapture();
    result=WakeVoice::Continue;
    while(result==WakeVoice::Continue)result=live(1000);
    assert(result==WakeVoice::Finish&&preN<PREBUF_N&&preHead==PREBUF_N-80);
    // Talking immediately after enabling leaves no quiet calibration prefix.
    seed(0,240,0);
    // A single loud pop must not inflate the calibration cap above the speech.
    chunk(reinterpret_cast<int16_t*>(prebuf+size_t((preHead+5)%PREBUF_N)*CHUNK_BYTES),32767);
    beginWakeInitialCapture();
    assert(s_wake_endpoint.heardSpeech());
    for(unsigned i=0;i<99;++i)assert(live(20)==WakeVoice::Continue);
    assert(live(20)==WakeVoice::Finish);
    puts("Initial wake capture: late hit, wrapped pre-roll, continuous/short question, name-only, silence, cap passed");
}
