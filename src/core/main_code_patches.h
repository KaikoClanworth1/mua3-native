#pragma once
// MUA3 main NSO (build id e853c44fdf18b88f...) frame-rate patches, shared by the runtime (core/game_patches.cpp)
// and the static recompiler (tools/recomp.cpp), so the recompiled code is the patched code and its hash matches.
//
// Each patched instruction is a MOV of a constant that becomes an LDR (literal) of a slot in the inter-function
// padding right after it (zero words no code reads). The code is the same whatever the configuration; only the
// slot values (written by the runtime) change, so the frame rate stays configurable without recompiling.
#include <cstdint>

namespace mua3patches {

constexpr std::uint64_t kMainLoadBase = 0x8004000; // where the recompiler maps main (the runtime uses the real base)

struct CodePatch {
    std::uint64_t site;     // main-relative offset of the MOV
    std::uint32_t original; // its instruction word
    std::uint64_t slot;     // main-relative offset of the literal (zero padding)
    bool wide;              // 64-bit register / slot
    std::uint32_t rt;
    constexpr std::uint32_t Ldr() const { // LDR Wt/Xt, <slot>
        const std::int64_t imm19 = (std::int64_t(slot) - std::int64_t(site)) / 4;
        return (wide ? 0x58000000u : 0x18000000u) | ((std::uint32_t(imm19) & 0x7FFFFu) << 5) | rt;
    }
};

// KTGL timing block (pointer at main+0x2188a00) +0x60 = present interval (2 = 30 fps, 1 = 60 fps): its
// constructor sets {2, 2} with "mov x9, #0x200000002", the game sets 2 again with "mov w9, #2".
// The frame limiter's target rate is stored once at startup from "mov w8, #0x41f00000" (30.0f).
constexpr CodePatch kInitInterval{0x8403b0, 0xB21F03E9, 0x8403f8, true, 9};
constexpr CodePatch kSetInterval{0x41eb48, 0x321F03E9, 0x41ebd8, false, 9};
constexpr CodePatch kLimiterTarget{0x839eb8, 0x52A83E08, 0x839f7c, false, 8};
constexpr CodePatch kCodePatches[] = {kInitInterval, kSetInterval, kLimiterTarget};

static_assert(kInitInterval.slot % 8 == 0 && kInitInterval.slot - kInitInterval.site < (1u << 20));

} // namespace mua3patches
