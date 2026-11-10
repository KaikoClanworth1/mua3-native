// Extensions to the game's own menus (PC-style options), made of host functions standing in for a few of the
// menu class's functions. See menu_mods.cpp and docs/game_menus.md for how the game builds its menus.
#pragma once
#include "common/common.h"

namespace core {
class Memory;
}
namespace kernel {
class Kernel;
}

namespace hle::menu {

// Hooks the main menu (the pause / S.H.I.E.L.D. point menu) of the main module loaded at `main_base`. Does nothing
// when the module is not the build these offsets were taken from (4.0.1, build id e853c44fdf18b88f) or with
// MUA3_MENU_MODS=0.
void Install(core::Memory& mem, kernel::Kernel& kernel, u64 main_base);

// RomFS reads: replaces the parts of game files that menu extensions draw anew (labels). `off` = offset in the file.
void PatchRomfsRead(const fs::path& host, u64 off, u8* dst, size_t n);

} // namespace hle::menu
