#include "../BendeRadio/WakeSample.h"
using namespace WakeSample;
constexpr uint32_t u32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
constexpr bool validWav() {
    uint8_t header[46] = {};
    header[44] = 0x5a; header[45] = 0xa5;
    wavHeader(header);
    return header[0] == 'R' && header[8] == 'W' && header[36] == 'd' &&
        u32(header + 4) == 192036 && u32(header + 16) == 16 &&
        header[20] == 1 && header[22] == 1 && u32(header + 24) == 24000 &&
        u32(header + 28) == 48000 && header[32] == 2 && header[34] == 16 &&
        u32(header + 40) == 192000 && header[44] == 0x5a && header[45] == 0xa5;
}
constexpr bool ids() {
    uint32_t n = 0;
    return parseId("4294967295", n) && n == UINT32_MAX && !parseId("4294967296", n) &&
        !parseId("-1", n) && !parseId("1x", n) && !parseId("0", n) && !parseId("", n) &&
        !parseId(nullptr, n) && !parseId("9999999999999999999999999999999", n);
}
static_assert(validWav(), "WAV layout and declared length must match the 4-second buffer");
static_assert(ids(), "Reject malformed, zero and overflowing sample IDs");
static_assert(active(Queued) && active(Recording) && !active(Ready) && !active(Failed) && !active(Idle));

constexpr bool dcAndRecovery() {
    MicFilter filter;
    // Raw values can exceed PCM16; a constant bias must not become clipping.
    for (int i = 0; i < 100; ++i) if (filter.process(120000.f) != 0) return false;
    if (filter.process(-120000.f) != -32767) return false;
    int16_t last = 0;
    for (int i = 0; i < 1500; ++i) last = filter.process(-120000.f);
    if (last < -1 || last > 1) return false;
    MicFilter next;
    return next.process(-50000.f) == 0 && next.process(-49000.f) > 980;
}
constexpr int64_t filteredEnergy(int period) {
    MicFilter filter;
    int64_t energy = 0;
    for (int i = 0; i < 30000; ++i) {
        // Bias alone would saturate the old conversion for every sample.
        int64_t value = filter.process(100000.f + (i % period < period / 2 ? 1000.f : -1000.f));
        if (i >= 6000) energy += value * value;
    }
    return energy;
}
static_assert(dcAndRecovery(), "Remove DC before clipping, recover after steps, reset between recordings");
static_assert(filteredEnergy(24) > int64_t(24000) * 500000, "Preserve 1 kHz content despite a large DC offset");
static_assert(filteredEnergy(8000) < int64_t(24000) * 100000, "Suppress slow 3 Hz microphone drift");

constexpr bool poolLifecycle() {
    Pool pool;
    uint8_t buffers[poolCapacity + 1] = {};
    Status items[poolCapacity] = {};
    if (pool.add(0, 0, 0, buffers) || pool.add(1, uint8_t(labelCount), 0, buffers) || pool.add(1, 0, 2, buffers)) return false;
    if (!pool.add(1, 0, 1, buffers) || pool.add(1, 1, 0, buffers + 1)) return false;
    if (pool.list(items, poolCapacity) != 0 || pool.count() != 1) return false;
    pool.find(1)->ready = true;
    for (size_t i = 1; i < poolCapacity; ++i) {
        if (!pool.add(uint32_t(i + 1), 1, 0, buffers + i)) return false;
        pool.find(uint32_t(i + 1))->ready = true;
    }
    if (pool.add(99, 0, 0, buffers + poolCapacity) || pool.count() != poolCapacity) return false;
    if (pool.list(items, poolCapacity) != poolCapacity || items[0].id != 1 || items[0].split != 1) return false;
    if (pool.list(items, 2) != 2 || pool.remove(99) != nullptr) return false;
    if (pool.remove(2) != buffers + 1 || pool.find(2) != nullptr || pool.find(1)->wav != buffers) return false;
    if (!pool.add(100, 3, 1, buffers + 1) || pool.list(items, poolCapacity) != poolCapacity - 1) return false;
    pool.find(100)->ready = true;
    if (pool.list(items, poolCapacity) != poolCapacity || items[1].label != 3) return false;
    // Cancelling a new/incomplete capture never removes older completed ones.
    if (pool.remove(100) != buffers + 1 || !pool.find(1)->ready) return false;
    return pool.count() == poolCapacity - 1;
}
static_assert(poolLifecycle(), "Pool must preserve previous records, reject overflow and omit incomplete captures");
