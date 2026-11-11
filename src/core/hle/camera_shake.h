// Camera shake at frame rates above 60 (see camera_shake.cpp).
#pragma once
#include "common/common.h"

namespace core {
class Memory;
struct Config;
} // namespace core
namespace kernel {
class Kernel;
}

namespace hle::camera {
void Install(core::Memory& mem, kernel::Kernel& kernel, u64 main_base, const core::Config& cfg);
}
