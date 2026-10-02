#pragma once
#include <cstdint>

struct BatteryAdcReading { uint16_t mv; uint16_t spread; uint16_t raw_spread; };

// Average eight short groups first; discard the two lowest/highest group means.
// A single noisy ADC conversion must not veto the entire battery measurement.
constexpr BatteryAdcReading battery_adc_reading(const uint16_t (&samples)[64]) {
    uint16_t means[8]{};
    uint16_t lo = 65535, hi = 0;
    for (unsigned g=0; g<8; ++g) {
        uint32_t sum = 0;
        for (unsigned j=0; j<8; ++j) {
            const auto v = samples[g*8+j];
            sum += v;
            if (v<lo) lo=v;
            if (v>hi) hi=v;
        }
        means[g] = (sum+4)/8;
    }
    for (unsigned i=1; i<8; ++i) {
        const auto v=means[i];
        unsigned j=i;
        while (j && means[j-1]>v) { means[j]=means[j-1]; --j; }
        means[j]=v;
    }
    const uint32_t sum = uint32_t(means[2])+means[3]+means[4]+means[5];
    return {uint16_t((sum+2)/4), uint16_t(means[5]-means[2]), uint16_t(hi-lo)};
}
