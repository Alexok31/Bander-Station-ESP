#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>
#include "WakeDspData.h"

// Portable streaming implementation of bender-logmel-v1. No allocation, I/O,
// training-time normalization or trimming. The capture supplies DC-filtered PCM24k.
namespace WakeFrontend {
constexpr unsigned bands=40, frames=238, inputSize=bands*frames;
class Stream {
public:
    void reset() {
        std::memset(history_,0,sizeof(history_)); std::memset(pcm_,0,sizeof(pcm_));
        // Features of zero PCM: (ln(1e-10)+10)/5.
        for (float& v : features_) v = -2.605170186f;
        inputCount_=outputTime_=0; decisionSamples_=0; pcmHead_=featureHead_=hop_=0;
    }
    // Returns true once per 100 ms of contiguous 16 kHz output.
    bool push24(int16_t sample) {
        history_[inputCount_%32]=float(sample)/32768.f;
        bool inference=false;
        if (outputTime_<=2*inputCount_) {
            float y=0;
            const unsigned phase=unsigned(outputTime_%2);
            const uint64_t newest=outputTime_/2;
            for (unsigned k=phase;k<61;k+=2) {
                const unsigned lag=(k-phase)/2;
                if (newest>=lag) y+=WakeDspData::fir[k]*history_[(newest-lag)%32];
            }
            // Discard 10 causal FIR outputs: compensate the centered filter's
            // 30 high-rate taps, matching scipy without looking into the future.
            if (outputTime_>=30) {
                // Dataset preparation stores the resampled waveform as PCM16
                // before extracting features. Preserve that rounding/clipping.
                float pcm=std::nearbyint(y*32768.f);
                pcm=pcm < -32768.f ? -32768.f : pcm > 32767.f ? 32767.f : pcm;
                inference=push16(pcm/32768.f);
            }
            outputTime_+=3;
        }
        ++inputCount_;
        return inference;
    }
    bool push16(float sample) {
        pcm_[pcmHead_]=sample; pcmHead_=(pcmHead_+1)%480;
        if (++hop_==160) { hop_=0; frame(); }
        // No 64-bit division in the per-sample hot path on a 32-bit MCU.
        if (++decisionSamples_==1600) { decisionSamples_=0; return true; }
        return false;
    }
    void copy(float* out) const {
        for (unsigned i=0;i<frames;++i)
            std::memcpy(out+i*bands,features_+((featureHead_+i)%frames)*bands,bands*sizeof(float));
    }
    void quantize(int8_t* out,float scale,int zero) const {
        for (unsigned i=0;i<frames;++i) for(unsigned j=0;j<bands;++j) {
            // nearbyint uses ties-to-even, as np.rint in offline evaluation.
            int q=int(std::nearbyint(features_[((featureHead_+i)%frames)*bands+j]/scale))+zero;
            out[i*bands+j]=int8_t(q < -128 ? -128 : q > 127 ? 127 : q);
        }
    }
private:
    void frame() {
        for(unsigned i=0;i<512;++i) {
            re_[i]=i<480 ? pcm_[(pcmHead_+i)%480]*WakeDspData::window[i] : 0.f;
            im_[i]=0.f;
        }
        for(unsigned i=1,j=0;i<512;++i) {
            unsigned bit=256; for(;j&bit;bit>>=1) j^=bit; j^=bit;
            if(i<j) {float t=re_[i];re_[i]=re_[j];re_[j]=t;}
        }
        for(unsigned len=2;len<=512;len<<=1) {
            const unsigned half=len/2,step=512/len;
            for(unsigned i=0;i<512;i+=len) for(unsigned j=0;j<half;++j) {
                const float c=WakeDspData::twiddle[2*j*step],s=WakeDspData::twiddle[2*j*step+1];
                const unsigned a=i+j,b=a+half;
                const float r=c*re_[b]-s*im_[b],v=s*re_[b]+c*im_[b];
                re_[b]=re_[a]-r; im_[b]=im_[a]-v; re_[a]+=r; im_[a]+=v;
            }
        }
        for(unsigned i=0;i<=256;++i) power_[i]=(re_[i]*re_[i]+im_[i]*im_[i])/512.f;
        unsigned offset=0;
        for(unsigned m=0;m<bands;++m) {
            float sum=0;
            for(unsigned j=0;j<WakeDspData::melLength[m];++j)
                sum+=power_[WakeDspData::melStart[m]+j]*WakeDspData::melWeight[offset++];
            float v=(std::log(sum>1e-10f?sum:1e-10f)+10.f)/5.f;
            features_[featureHead_*bands+m]=v < -3 ? -3 : v > 3 ? 3 : v;
        }
        featureHead_=(featureHead_+1)%frames;
    }
    float history_[32],pcm_[480],features_[inputSize],re_[512],im_[512],power_[257];
    uint64_t inputCount_=0,outputTime_=0;
    unsigned pcmHead_=0,featureHead_=0,hop_=0,decisionSamples_=0;
};

}
