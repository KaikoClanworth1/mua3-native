// Menu auto-repeat at frame rates above 60 (see menu_repeat.cpp).
#pragma once
#include "common/common.h"

namespace core {
class Memory;
struct Config;
} // namespace core
namespace kernel {
class Kernel;
}

namespace hle::menu::repeat {
void Install(core::Memory& mem, kernel::Kernel& kernel, u64 main_base, const core::Config& cfg);
}
