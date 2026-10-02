#include "../BendeRadio/BatteryAdc.h"
#include "../BendeRadio/BatteryProtection.h"

constexpr bool noisy_connected_pack() {
    uint16_t samples[64]{};
    for (unsigned i=0;i<64;++i) samples[i]=uint16_t(2200 + (i%2 ? 190 : -190));
    const auto v=battery_adc_reading(samples);
    return v.mv==2200 && v.raw_spread==380 && v.spread==0 &&
           battery_sense_candidate(7309,v.spread,1000,9200,100);
}
static_assert(noisy_connected_pack(), "380 mV individual noise must not reject a stable group average");

constexpr bool outliers() {
    uint16_t samples[64]{};
    for (auto& v:samples) v=2200;
    samples[0]=0; samples[63]=3300;
    const auto v=battery_adc_reading(samples);
    return v.mv==2200 && v.spread==0 && v.raw_spread==3300;
}
static_assert(outliers(), "Isolated ADC glitches do not move the estimate");

constexpr bool unstable_and_low() {
    uint16_t samples[64]{};
    for (unsigned i=0;i<64;++i) samples[i]=uint16_t(500+300*(i/8));
    if (battery_adc_reading(samples).spread<=100) return false;
    for (auto& v:samples) v=1600;
    if (battery_adc_reading(samples).mv!=1600) return false;
    for (auto& v:samples) v=0;
    return battery_adc_reading(samples).mv==0;
}
static_assert(unstable_and_low(), "Reject drifting group means but preserve sustained low/zero voltage");
