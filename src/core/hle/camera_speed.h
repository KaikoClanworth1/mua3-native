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
// MOUSE CAMERA (live): mouse counts turn the camera directly (sensitivity in percent; 100 = 0.0025 rad per count).
void SetMouse(bool on, int sensitivity, bool invert_y);
// Mouse counts (x right, y down) to add at the camera's next update.
void AddMouse(float dx, float dy);
// The player camera updated in the last 150 ms (gameplay, not a menu, pause or cut-scene): the mouse may be captured.
bool CameraActive();
} // namespace hle::camera_speed
