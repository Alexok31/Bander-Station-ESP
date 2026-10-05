#include "../BendeRadio/WakeVoice.h"
#include <cassert>
#include <cstdio>

int main() {
    WakeVoice::Endpoint endpoint;
    int16_t quiet[240]{}, dc[240], speech[240], soft[240];
    for (int i=0;i<240;++i) {
        dc[i]=3000; speech[i]=3000+(i%2?1200:-1200); soft[i]=i%2?60:-60;
    }
    // Silence, DC offset and low background must not cause an ASR commit.
    for (auto pcm : {quiet,dc,soft}) {
        endpoint.reset();
        for (int i=0;i<599;++i) assert(endpoint.push(pcm,240)==WakeVoice::Continue);
        assert(endpoint.push(pcm,240)==WakeVoice::Cancel);
        assert(!endpoint.heardSpeech());
    }
    // Isolated loud clicks do not qualify as a spoken question.
    endpoint.reset();
    for (int i=0;i<599;++i) assert(endpoint.push(i%30==0?speech:quiet,240)==WakeVoice::Continue);
    assert(endpoint.push(quiet,240)==WakeVoice::Cancel);
    // Question with a short internal pause: only a full second of silence ends it.
    endpoint.reset();
    for (int i=0;i<40;++i) assert(endpoint.push(speech,240)==WakeVoice::Continue);
    for (int i=0;i<50;++i) assert(endpoint.push(quiet,240)==WakeVoice::Continue);
    for (int i=0;i<30;++i) assert(endpoint.push(speech,240)==WakeVoice::Continue);
    assert(endpoint.heardSpeech() && endpoint.voicedMs()==700);
    for (int i=0;i<99;++i) assert(endpoint.push(quiet,240)==WakeVoice::Continue);
    assert(endpoint.push(quiet,240)==WakeVoice::Finish);
    // Continuous sound is bounded; no unbounded recording/PSRAM accumulation.
    endpoint.reset();
    for (int i=0;i<1499;++i) assert(endpoint.push(speech,240)==WakeVoice::Continue);
    assert(endpoint.push(speech,240)==WakeVoice::Finish);
    // A new interaction cannot inherit previous speech or a silence timer.
    endpoint.reset();
    assert(!endpoint.heardSpeech() && endpoint.voicedMs()==0);
    for (int i=0;i<599;++i) assert(endpoint.push(quiet,240)==WakeVoice::Continue);
    assert(endpoint.push(quiet,240)==WakeVoice::Cancel);
    // Real-device failure: a 300–900 RMS noise floor exceeds the old fixed 250.
    // Calibrate the same gain/path; intermittent louder background must not
    // keep the question open for the full 15 seconds.
    int16_t background[240],loud[240];
    for(int i=0;i<240;++i)loud[i]=i%2?4000:-4000;
    auto noise=[&](int level){for(int i=0;i<240;++i)background[i]=i%2?level:-level;};
    auto calibrate=[&](){
        for(int i=0;i<50;++i){noise(300+(i%5)*150);endpoint.observeBackground(background,240);}
        endpoint.finishCalibration();
        assert(endpoint.noise()==450&&endpoint.threshold()==1125);
    };
    endpoint.reset();calibrate();
    for(int i=0;i<80;++i)assert(endpoint.push(loud,240)==WakeVoice::Continue);
    for(int i=0;i<99;++i){noise(300+(i%5)*150);assert(endpoint.push(background,240)==WakeVoice::Continue);}
    noise(600);assert(endpoint.push(background,240)==WakeVoice::Finish);
    endpoint.reset();calibrate();
    for(int i=0;i<599;++i){noise(300+(i%5)*150);assert(endpoint.push(background,240)==WakeVoice::Continue);}
    assert(endpoint.push(background,240)==WakeVoice::Cancel);
    // Sporadic early speech during calibration doesn't set the noise floor.
    endpoint.reset();
    for(int i=0;i<50;++i){noise(i<15?4000:500);endpoint.observeBackground(background,240);}
    endpoint.finishCalibration();assert(endpoint.noise()==500);
    // Long follow-up window does not shorten the question's own 15s budget.
    endpoint.reset(30000);calibrate();
    for(int i=0;i<2000;++i){noise(450);assert(endpoint.push(background,240)==WakeVoice::Continue);}
    for(int i=0;i<1499;++i)assert(endpoint.push(loud,240)==WakeVoice::Continue);
    assert(endpoint.push(loud,240)==WakeVoice::Finish);
    endpoint.reset(30000);calibrate();noise(450);
    for(int i=0;i<2999;++i)assert(endpoint.push(background,240)==WakeVoice::Continue);
    assert(endpoint.push(background,240)==WakeVoice::Cancel);
    puts("WakeVoice endpoint: silence/DC/noise, clicks, phrase pauses, cap, reset passed");
}
