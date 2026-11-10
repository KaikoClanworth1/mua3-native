// The in-game GRAPHICS page: the game's OPTIONS screen showing the PC display settings (mua3.ini [video]).
#pragma once
#include "common/common.h"

namespace core {
class Memory;
}
namespace kernel {
class Kernel;
}
namespace recomp {
struct State;
}

namespace hle::menu::graphics {

// Hooks the OPTIONS screen of the main module at `main_base` (build checked by the caller).
void Install(core::Memory& mem, kernel::Kernel& kernel, u64 main_base);

// The next OPTIONS screen that opens shows the GRAPHICS page (call, then open OPTIONS the game's way).
void OpenNext();

// A menu label for the GRAPHICS item (system string hash): the game's own "GRAPHICS" text.
u32 LabelHash();

} // namespace hle::menu::graphics
