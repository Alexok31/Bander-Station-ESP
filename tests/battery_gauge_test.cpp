#include "../BendeRadio/BatteryGauge.h"

static_assert(battery_voltage_percent(0,8400) == 0);
static_assert(battery_voltage_percent(6000,8400) == 0);
static_assert(battery_voltage_percent(7400,8400) == 46, "Interpolation between 42 and 50");
static_assert(battery_voltage_percent(8200,8400) == 84);
static_assert(battery_voltage_percent(8400,8400) == 99, "Nominal 2S upper display point");
static_assert(battery_voltage_percent(9000,8400) == 99);
static_assert(battery_voltage_percent(8400,8500) == 94, "Full voltage is configurable");
constexpr bool monotonic() {
    uint8_t previous = 0;
    for (uint16_t mv=0; mv<10000; ++mv) {
        const auto p = battery_voltage_percent(mv,8400);
        if (p<previous || p>99) return false;
        previous=p;
    }
    return true;
}
static_assert(monotonic());

constexpr bool settles_at_full() {
    uint16_t filtered = 8200;
    for (unsigned i=0; i<100; ++i) filtered = battery_display_filter(filtered,8400);
    return battery_voltage_percent(filtered,8400)==99;
}
static_assert(settles_at_full(), "Integer filter rounding must not hold a full pack at 98%");

constexpr bool charge_filter() {
    BatteryChargeDebounce f;
    if (f.sample(0,true) || f.sample(100,false)) return false;
    if (!f.sample(1000,true)) return false; // Two qualified bursts across LED blink.
    if (!f.sample(1900,false) || !f.sample(2000,true)) return false;
    if (!f.sample(4499,false) || f.sample(4500,false)) return false;
    BatteryChargeDebounce isolated;
    if (isolated.sample(0,true) || isolated.sample(2500,false)) return false;
    return !isolated.sample(5000,true);
}
static_assert(charge_filter(), "Blinking CHG stays active; an isolated burst does not assert charging");

constexpr bool plug_unplug() {
    BatteryGauge g;
    if(g.sample(0,40,false)!=40) return false;
    if(g.sample(1000,70,true)!=40) return false;
    if(g.sample(30999,70,true)!=40) return false;
    if(g.sample(31000,70,true)!=41) return false;
    if(g.sample(32000,40,false)!=41) return false;
    if(g.sample(61999,40,false)!=41) return false;
    return g.sample(62000,40,false)==40;
}
static_assert(plug_unplug(), "Connection transients cannot jump the displayed SOC");

constexpr bool manual_and_long_gap() {
    BatteryGauge g;
    g.sample(0,50,true);
    if(g.sample(30000,90,true)!=51) return false;
    for(uint32_t t=30001; t<50000; t+=100)
        if(g.sample(t,90,true)!=51) return false;
    if(g.sample(50000,90,true)!=52) return false;
    return g.sample(3600000,90,true)==53;
}
static_assert(manual_and_long_gap(), "Manual requests and gaps cannot fast-forward percentage");

constexpr bool load_exceeds_charge() {
    BatteryGauge g;
    g.sample(0,60,true);
    if(g.sample(30000,40,true)!=59) return false;
    if(g.sample(39999,40,true)!=59) return false;
    return g.sample(40000,40,true)==58;
}
static_assert(load_exceeds_charge(), "Do not invent a rising charge while the battery is draining");

constexpr bool wrap() {
    BatteryGauge g;
    g.sample(UINT32_MAX-999,50,false);
    if(g.sample(28999,20,false)!=50) return false;
    if(g.sample(29000,20,false)!=49) return false;
    BatteryChargeDebounce f;
    f.sample(UINT32_MAX-999,true);
    if (!f.sample(UINT32_MAX-899,true)) return false;
    return f.sample(1599,false) && !f.sample(1600,false);
}
static_assert(wrap());
