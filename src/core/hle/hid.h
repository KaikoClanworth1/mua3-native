#pragma once
#include "common/common.h"

namespace hle::hid {

enum Button : u64 {
    A = 1ull << 0, B = 1ull << 1, X = 1ull << 2, Y = 1ull << 3,
    LStick = 1ull << 4, RStick = 1ull << 5, L = 1ull << 6, R = 1ull << 7,
    ZL = 1ull << 8, ZR = 1ull << 9, Plus = 1ull << 10, Minus = 1ull << 11,
    Left = 1ull << 12, Up = 1ull << 13, Right = 1ull << 14, Down = 1ull << 15,
    LStickLeft = 1ull << 16, LStickUp = 1ull << 17, LStickRight = 1ull << 18, LStickDown = 1ull << 19,
    RStickLeft = 1ull << 20, RStickUp = 1ull << 21, RStickRight = 1ull << 22, RStickDown = 1ull << 23,
};

struct PadInput {
    bool connected = false;
    u64 buttons = 0;
    s32 lx = 0, ly = 0, rx = 0, ry = 0; // -32767..32767, +y = up
};

// Called by the platform layer (any thread); written into HID shared memory by the HID update thread.
void SetPad(int player, const PadInput& in);
void StartUpdateThread();

} // namespace hle::hid
