// PC SETTINGS > CAMERA SPEED: the game's own camera turn speed, scaled (see camera_speed.cpp).
#pragma once
#include "common/common.h"

namespace core {
class Memory;
} // namespace core
namespace kernel {
class Kernel;
}

namespace hle::camera_speed {
void Install(core::Memory& mem, kernel::Kernel& kernel, u64 main_base, int percent);
// live: 10-200 (100 = the game's speed)
void Set(int percent);
} // namespace hle::camera_speed
