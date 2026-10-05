#include "../BendeRadio/WakeFrontend.h"
#include "../BendeRadio/WakeDecision.h"
#include <cstdio>
#include <cassert>
int main(int argc,char** argv) {
    WakeDecision::Cadence cadence;
    for(int i=0;i<20;++i)assert(cadence.advance()==bool(i%2));
    assert(!cadence.advance());cadence.reset();assert(!cadence.advance());assert(cadence.advance());
    WakeDecision::Trigger trigger;
    assert(!trigger.step(.5f));assert(trigger.step(.5f));
    for(unsigned i=0;i<WakeDecision::refractoryMs/WakeDecision::intervalMs;++i)assert(!trigger.step(1.f));
    assert(!trigger.step(1.f));assert(trigger.step(1.f));
    trigger.reset();assert(!trigger.step(.5f));assert(!trigger.step(0));
    assert(!trigger.step(.5f));assert(trigger.step(.5f));
    if(argc!=4)return 2;
    FILE* in=std::fopen(argv[1],"rb");FILE* out=std::fopen(argv[2],"wb");
    FILE* quant=std::fopen(argv[3],"wb");
    if(!in||!out||!quant)return 3;
    WakeFrontend::Stream stream;stream.reset();
    float features[WakeFrontend::inputSize];int8_t values[WakeFrontend::inputSize];int16_t sample;
    while(std::fread(&sample,sizeof(sample),1,in)==1) {
        if(stream.push24(sample)) {
            stream.copy(features);
            if(std::fwrite(features,sizeof(features),1,out)!=1)return 4;
            stream.quantize(values,.022312475368380547f,6);
            if(std::fwrite(values,sizeof(values),1,quant)!=1)return 4;
        }
    }
    stream.reset();stream.quantize(values,.022312475368380547f,6);
    for(auto v:values)assert(v==-111); // Discontinuities must erase previous speech.
    std::fclose(in);std::fclose(out);std::fclose(quant);return 0;
}
