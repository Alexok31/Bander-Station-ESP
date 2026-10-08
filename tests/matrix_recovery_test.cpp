#include "../BendeRadio/MatrixRecovery.h"
#include <array>
#include <cassert>
#include <cstdio>

int main() {
    MatrixRecoverySchedule timer;
    assert(!timer.due(99999));
    timer.start(100);
    assert(!timer.due(599));
    assert(timer.due(600));
    assert(!timer.due(600));
    for (uint32_t now = 1100; now < 10100; now += 500) assert(timer.due(now));
    assert(!timer.due(10100));
    assert(timer.due(24600));
    timer.start(UINT32_MAX - 200);
    assert(!timer.due(298));
    assert(timer.due(299));

    std::array<uint8_t, 16> registers{};
    const std::array<uint8_t, 5> brightness{3, 4, 5, 6, 7};
    std::array<uint8_t, 5> hardwareBrightness{};
    std::array<uint8_t, 40> image{}, display{};
    image[3] = 0x81;
    image[27] = 0x7e;
    bool powered = false;
    auto recover = [&] {
        matrix_restore_registers([&](uint8_t r, uint8_t v) {
            if (!powered) return;
            if (r == 0x0c) { assert(v == 1); assert(display == image); }
            registers[r] = v;
        }, [&] { if (powered) hardwareBrightness = brightness; },
        [&] { if (powered) display = image; });
    };
    recover(); // ESP starts while the matrix rail is still down.
    assert(registers[0x0c] == 0);
    powered = true;
    recover(); // Later retry must restore controls, brightness and the same frame.
    assert(registers[0x0c] == 1 && registers[0x0b] == 7);
    assert(registers[0x09] == 0 && registers[0x0f] == 0);
    assert(hardwareBrightness == brightness && display == image);
    registers.fill(0); display.fill(0); // Driver resets after startup.
    recover();
    assert(registers[0x0c] == 1 && display == image);
    recover(); // Healthy refresh neither blanks nor changes the frame.
    assert(display == image);
    puts("matrix recovery: OK");
}
