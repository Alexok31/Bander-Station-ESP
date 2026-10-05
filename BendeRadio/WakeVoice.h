#pragma once
#include <cstddef>
#include <cstdint>
#include <cmath>
#include <algorithm>

// Endpointing for the question AFTER the wake phrase. Audio stays local until
// Finish; Cancel never commits silence to ASR. Called on 10 ms PCM24k chunks.
namespace WakeVoice {
enum Result { Continue, Finish, Cancel };
class Endpoint {
    uint32_t elapsed_ = 0, voiced_ = 0, run_ = 0, quiet_ = 0, lastActivity_ = 0;
    uint32_t noSpeechMs_ = 6000;
    uint32_t speechStarted_ = 0;
    float noise_ = 80;
    float rms_ = 0, threshold_ = 250;
    bool heard_ = false;
    float background_[50]{};
    unsigned backgroundCount_ = 0;
    static float acRms(const int16_t* pcm, size_t count) {
        if (!count) return 0;
        int64_t sum=0,squares=0;
        for(size_t i=0;i<count;++i){const int64_t x=pcm[i];sum+=x;squares+=x*x;}
        const float mean=float(sum)/count;
        const float variance=float(squares)/count-mean*mean;
        return std::sqrt(variance>0?variance:0);
    }
public:
    static float level(const int16_t* pcm,size_t count) { return acRms(pcm,count); }
    void reset(uint32_t noSpeechMs=6000) { *this=Endpoint{};noSpeechMs_=noSpeechMs; }
    // Same PCM path/gain as the question, speaker muted, before the ready cue.
    // Lower quartile rejects brief bumps/early speech during calibration.
    void observeBackground(const int16_t* pcm,size_t count) {
        if(backgroundCount_<50) background_[backgroundCount_++]=acRms(pcm,count);
    }
    void finishCalibration(float maximumNoise=1e9f) {
        if(backgroundCount_) {
            std::sort(background_,background_+backgroundCount_);
            noise_=std::max(80.f,std::min(maximumNoise,background_[backgroundCount_/4]));
        }
        threshold_=std::max(500.f,noise_*2.5f);
    }
    bool heardSpeech() const { return heard_; }
    uint32_t voicedMs() const { return voiced_; }
    uint32_t elapsedMs() const { return elapsed_; }
    float rms() const { return rms_; }
    float threshold() const { return threshold_; }
    float noise() const { return noise_; }
    uint32_t quietMs() const { return quiet_; }
    Result push(const int16_t* pcm, size_t count) {
        if (!count) return Continue;
        const float rms=acRms(pcm,count);
        // Lower end threshold preserves quiet syllables after speech begins.
        const float threshold=heard_?std::max(400.f,noise_*1.8f):std::max(500.f,noise_*2.5f);
        rms_ = rms; threshold_ = threshold;
        elapsed_ += 10;
        if (rms >= threshold) {
            run_ += 10;
            // Short clicks don't count as speech; require 120 ms continuity.
            if (run_ == 120) voiced_ += 120;
            else if (run_ > 120) voiced_ += 10;
            if (voiced_ >= 250 && !heard_) {heard_=true;speechStarted_=elapsed_-run_;}
            // Isolated clicks don't restart the silence timeout.
            if(run_>=60) lastActivity_=elapsed_;
        } else {
            run_ = 0;
        }
        quiet_=heard_?elapsed_-lastActivity_:0;
        if (heard_ && quiet_ >= 1000) return Finish;
        if (!heard_ && elapsed_ >= noSpeechMs_) return Cancel;
        if (heard_ && uint32_t(elapsed_-speechStarted_) >= 15000) return Finish;
        return Continue;
    }
};
}
