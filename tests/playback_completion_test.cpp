#include "../BendeRadio/PlaybackCompletion.h"
#include <cassert>
#include <cstdio>

using BenderPlayback::readyToRelease;
int main() {
    // Replay the deadlock: underrun unprimes the worker at t=450, before
    // response.done/input freshness expires. Main-loop completion must still fire.
    BenderPlayback::State s{true,false,false,true,true,false,false,0,450,100,100};
    assert(!readyToRelease(s));
    s.now=500;s.idle=true;s.speaking=false;s.waiting=false; // response.done
    assert(!readyToRelease(s));
    s.now=899;assert(!readyToRelease(s));
    s.now=900;assert(readyToRelease(s));
    // A second and third turn must use their own timestamps and drain fully.
    for (unsigned turn=1;turn<=3;++turn) {
        s.now=turn*10000;s.lastInputMs=s.now;s.lastOutputMs=s.now;
        s.idle=false;s.waiting=true;s.queuedBytes=480;
        assert(!readyToRelease(s));
        s.now+=1000;s.idle=true;s.waiting=false;
        assert(!readyToRelease(s)); // response.done doesn't discard buffered speech
        s.queuedBytes=0;s.inFlight=true;assert(!readyToRelease(s));
        s.lastOutputMs=s.now;s.inFlight=false;assert(!readyToRelease(s));
        s.now+=349;assert(!readyToRelease(s));
        ++s.now;assert(readyToRelease(s));
    }
    s.speaking=true;assert(!readyToRelease(s));s.speaking=false;
    s.waiting=true;assert(!readyToRelease(s));s.waiting=false;
    s.debug=true;assert(!readyToRelease(s));s.debug=false;
    s.taking=true;assert(!readyToRelease(s));s.taking=false;
    s.ownsSpeaker=false;assert(!readyToRelease(s));s.ownsSpeaker=true;
    // A valid text-only/empty response also releases the speaker.
    s.lastInputMs=s.lastOutputMs=0;assert(readyToRelease(s));
    // millis rollover must not wedge the speaker or release its DMA tail early.
    s.lastInputMs=UINT32_MAX-900;s.lastOutputMs=UINT32_MAX-100;s.now=200;
    assert(!readyToRelease(s));s.now=250;assert(readyToRelease(s));
    puts("Playback completion: underrun/late done, repeated turns, DMA tail, empty reply, rollover passed");
}
