#include "../BendeRadio/WakeReply.h"
#include "../BendeRadio/WakeReplyAudio.h"
#include <atomic>
#include <cassert>
#include <cstring>
#include <algorithm>
#include <cstdio>
constexpr size_t CHUNK=240;
static bool s_wake_followup;
static std::atomic<bool> s_wake_cue{false};
static BenderCharacter::Settings traits;
static size_t sent;
static uint8_t expected;
static bool muted;
#define ALOG(...) ((void)0)
struct AirPlayAudioGuard {};
auto nvsLoadCharacter(){return traits;}
uint32_t esp_random(){return 0;}
size_t minOf(size_t a,size_t b){return std::min(a,b);}
void ampMuteHw(bool value){muted=value;}
void i2sWriteSilenceChunks(unsigned count){assert(count>=4);}
void spkWrite(uint8_t* data,size_t size,bool fromPcm){
    assert(s_wake_cue.load()&&fromPcm&&size%2==0&&size<=CHUNK*2);
    const auto& clip=WakeReplyAudio::clips[expected];
    assert(sent+size<=clip.bytes);
    assert(memcmp(data,clip.data+sent,size)==0);
    sent+=size;
    // Production volume scales this buffer. Verify the source is never modified.
    memset(data,0,size);
}
#include "../artifacts/wake-runtime/wake_reply_under_test.h"

int main(){
    using namespace WakeReply;
    BenderCharacter::Settings low;
    for(auto& v:low.values)v=0;
    assert(bankFor(low,false)==Neutral);
    const unsigned sliders[]={4,0,5,2,6};
    const Bank banks[]={Warm,Sarcastic,Rough,Curious,Profane};
    for(unsigned i=0;i<5;++i){
        auto profile=low;profile.values[sliders[i]]=100;
        assert(bankFor(profile,false)==banks[i]);
        assert(bankFor(profile,true)==Neutral);
        uint8_t previous=255;
        for(unsigned n=0;n<20;++n){
            const auto choice=choose(profile,false,n,previous);
            assert(choice<WakeReplyAudio::count&&choice!=previous);
            assert(choice/2==banks[i]);previous=choice;
        }
    }
    auto p=low;p.values[6]=84;assert(bankFor(p,false)!=Profane);
    p.values[6]=85;assert(bankFor(p,false)==Profane);
    size_t total=0;
    for(const auto& clip:WakeReplyAudio::clips){
        assert(clip.bytes%2==0&&clip.bytes<=24000*2*3);
        total+=clip.bytes;
    }
    assert(total<1000000);
    traits=low;
    for(uint8_t i=0;i<4;++i){
        expected=i%2;sent=0;muted=false;
        playWakeReadyCue();
        assert(sent==WakeReplyAudio::clips[expected].bytes&&muted&&!s_wake_cue.load());
    }
    traits.values[0]=100;expected=4;sent=0;playWakeReadyCue();
    assert(sent==WakeReplyAudio::clips[expected].bytes);
    s_wake_followup=true;sent=0;muted=false;playWakeReadyCue();
    assert(sent==0&&!muted&&!s_wake_cue.load());
    // Silent follow-up must not consume a variant or suppress the next wake cue.
    s_wake_followup=false;expected=5;sent=0;playWakeReadyCue();
    assert(sent==WakeReplyAudio::clips[expected].bytes);
    puts("Local wake replies: character, profanity gate, variety, follow-up, flash PCM streaming passed");
}
