// Helpers shared by the menu extensions (menu_mods.cpp, menu_graphics.cpp): host functions standing in for game
// functions, and guest calls made from them.
#pragma once
#include <cstring>
#include <initializer_list>

#include "core/cpu/cpu.h"
#include "core/cpu/recomp/recomp_rt.h"
#include "core/system.h"

namespace hle::menu {

// Calls guest code from a host function with the guest calling convention and returns x0; the guest registers are
// restored afterwards. `h` = run that overridden function's own recompiled code.
inline constexpr u64 kHostReturn = 0xFFFF'FFFF'FFFF'F000ull;
inline u64 GuestCall(recomp::State& s, u64 pc, std::initializer_list<u64> args,
                     const cpu::FunctionOverride* h = nullptr) {
    recomp::State saved;
    std::memcpy(&saved, &s, sizeof(recomp::State));
    int i = 0;
    for (u64 a : args)
        s.x[i++] = a;
    s.x[30] = kHostReturn;
    s.pc = pc;
    if (const auto fn = h ? h->original.load(std::memory_order_acquire) : nullptr) {
        if (h->direct)
            *h->ok = 1;
        fn(s);
        if (h->direct)
            *h->ok = 0;
    }
    while (s.pc != kHostReturn) { // tail jumps and exits to other functions
        const u64 at = s.pc;
        recomp::Dispatch(s);
        if (s.pc == at)
            break;
    }
    const bool done = s.pc == kHostReturn;
    const u64 r = s.x[0];
    const u64 icount = s.icount;
    std::memcpy(&s, &saved, sizeof(recomp::State));
    s.icount = icount;
    if (!done)
        LOG_ERROR("Menu", "guest call to {} did not return (stopped at {:#x})", core::Sys().Symbolize(pc), saved.pc);
    return r;
}

// Runs the overridden function itself for the caller (arguments and return address as they are).
inline void RunOriginal(recomp::State& s, const cpu::FunctionOverride& o) {
    if (o.direct)
        *o.ok = 1;
    o.original.load(std::memory_order_acquire)(s);
    if (o.direct)
        *o.ok = 0;
}

// Returns from the overridden function with x0 = v.
inline void GuestReturn(recomp::State& s, u64 v) {
    s.x[0] = v;
    s.pc = s.x[30];
    s.icount++;
}

} // namespace hle::menu
