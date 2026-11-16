// hle::mods::Enabled(), apart from mods.cpp: it needs the running game's settings, and MUA3Setup (which has no running
// game) compiles mods.cpp for its Mods page.
#include <cstdlib>

#include "core/hle/mods.h"
#include "core/system.h"

namespace hle::mods {

bool Enabled() {
    if (const char* e = std::getenv("MUA3_MODS"); e && e[0] == '0')
        return false;
    return core::Sys().Cfg().mods_enabled;
}

} // namespace hle::mods
