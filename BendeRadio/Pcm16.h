#pragma once

#include <cstddef>

// A byte ring reserves one byte to distinguish full/empty. Its free count may
// be odd; never copy half a mono PCM16 sample, even when dropping excess data.
constexpr size_t pcm16_whole_bytes(size_t bytes) {
    return bytes & ~size_t(1);
}
