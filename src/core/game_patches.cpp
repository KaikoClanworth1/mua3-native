#include "core/game_patches.h"

#include <cstring>

#include <algorithm>

#include "core/config.h"
#include "core/main_code_patches.h"
#include "core/memory.h"

namespace core {

namespace {

// MUA3 v3.0.0 main NSO (build id e853c44fdf18b88f...), loaded at a module base the patches are relative to.
constexpr u8 kMainBuildId[8] = {0xe8, 0x53, 0xc4, 0x4f, 0xdf, 0x18, 0xb8, 0x8f};

// Frame rate. KTGL keeps a timing block (pointer at main+0x2188a00) whose +0x60 is the present interval: its
// constructor and the game set 2 (30 fps), the engine also supports 1 (60 fps). Game systems read the interval and the frame
// delta (+0x18, clamped below by 1/60 or 1/30 from a table at main+0x19f33d8), so forcing interval 1 gives a
// correctly timed 60 fps. The frame limiter sleeps to a target rate stored once at startup (30.0f).
// The instructions are patched the same way for every configuration (core/main_code_patches.h: the recompiled
// code is built from the patched words); only the literal slots they load from depend on fps_target.
constexpr u64 kMinDeltaTable = 0x19f33d8; // float[2] {1/60, 1/30}: minimum frame delta per interval

bool Patch32(Memory& mem, u64 addr, u32 expect, u32 value, const char* what) {
    const u32 cur = mem.Read<u32>(addr);
    if (cur != expect) {
        LOG_WARN("Patch", "{}: unexpected word {:#010x} at {:#x} (expected {:#010x}); skipped", what, cur, addr, expect);
        return false;
    }
    mem.Write<u32>(addr, value);
    return true;
}

u32 FloatBits(float f) {
    u32 u;
    std::memcpy(&u, &f, 4);
    return u;
}

} // namespace

void ApplyGamePatches(Memory& mem, const std::string& module, u64 base, std::span<const u8> build_id, const Config& cfg) {
    if (module != "main" || build_id.size() < 8 || std::memcmp(build_id.data(), kMainBuildId, 8) != 0)
        return;
    using namespace mua3patches;
    for (const CodePatch& p : kCodePatches) {
        const u32 slot_words = p.wide ? 2 : 1;
        for (u32 k = 0; k < slot_words; k++)
            if (mem.Read<u32>(base + p.slot + k * 4) != 0) {
                LOG_WARN("Patch", "frame rate: padding at main+{:#x} is not free; no patches", p.slot);
                return;
            }
        if (mem.Read<u32>(base + p.site) != p.original) {
            LOG_WARN("Patch", "frame rate: unexpected code at main+{:#x}; no patches", p.site);
            return;
        }
    }
    const int fps = std::max(cfg.fps_target, 30);
    const bool unlocked = fps > 30;
    for (const CodePatch& p : kCodePatches)
        mem.Write<u32>(base + p.site, p.Ldr());
    mem.Write<u64>(base + kInitInterval.slot, unlocked ? 0x100000001ull : 0x200000002ull);
    mem.Write<u32>(base + kSetInterval.slot, unlocked ? 1 : 2);
    mem.Write<u32>(base + kLimiterTarget.slot, FloatBits(float(fps)));
    if (!unlocked)
        return;
    if (fps > 60) {
        // Above 60 the minimum frame delta (1/60) would speed the game up: lower it to one target frame.
        // Experimental: some systems may still assume 60 updates per second.
        Patch32(mem, base + kMinDeltaTable, FloatBits(1.0f / 60.0f), FloatBits(1.0f / float(fps)), "minimum frame delta");
    }
    LOG_INFO("Patch", "frame rate unlocked: target {} fps", fps);
}

} // namespace core
