// Menu auto-repeat at frame rates above 60.
//
// The game builds each controller's input record once per frame (main+0x176940, x0 = the record, which persists,
// w2 = the step: the present interval from KTGL's timing block, 2 at 30 fps and 1 at 60). For each of its 16
// buttons a held counter (u8 at record +0x34 + i; total held steps u32 at +0x44 + 4i) grows by the step; at 16 the
// button repeats (bit set in the "pressed or repeated" mask at +0xc, which menus read), at 20 it repeats and goes
// back to 16. So the first repeat comes after 16/60 s and then one every 4/60 s. Above 60 fps the interval stays 1,
// and the repeat ran fps/60 times too fast (2.4x at 144, 4x at 240): every menu was hard to steer.
//
// The builder is wrapped: each record counts 1/60 s ticks of frame time (+0x18, at most the interval, as the game
// itself would). A frame with whole ticks passes them as the step. A frame without one still runs the builder (the
// held / pressed bits are rebuilt every frame) with step 1, then gives back what that step did: the counters of
// buttons that were already held go back to their value before, and their repeat bit is cleared (a fresh press keeps
// its bit). Installed only when fps_target is above 60; at 30/60 nothing changes.
#include "core/hle/menu_repeat.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cmath>
#include <mutex>
#include <unordered_map>

#include "core/config.h"
#include "core/hle/menu_guest.h"
#include "core/kernel/kernel.h"
#include "core/memory.h"

namespace hle::menu::repeat {
namespace {

constexpr u64 kBuildRecord = 0x176940; // per-controller input record builder (x0 = record, w2 = step)
constexpr u64 kMasks = 0x19f7e90;      // {u32 mask, u32 button}[16] per record type (u32 at record +0xf8)
constexpr u64 kTimingPtr = 0x2188a00;  // KTGL timing block pointer: +0x18 frame time (s), +0x60 present interval
constexpr u32 kButtons = 16;

core::Memory* g_mem = nullptr;
u64 g_main = 0;
cpu::FunctionOverride g_hook;
std::mutex g_mutex;
std::unordered_map<u64, float> g_ticks; // record -> 1/60 s ticks not yet given to its counters
const bool g_trace = std::getenv("MUA3_TRACE_REPEAT") != nullptr; // test aid: log each repeat

// Test aid: the repeats (not fresh presses) a record reports this frame, with the time.
void Trace(u64 record) {
    const u32 repeat = g_mem->Read<u32>(record + 0xc) & ~g_mem->Read<u32>(record + 8);
    if (!repeat)
        return;
    static const auto t0 = std::chrono::steady_clock::now();
    const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    LOG_INFO("Menu", "repeat {:#x} at {:.3f} s (record {:#x})", repeat, t, record);
}

void BuildRecord(recomp::State& s) {
    const u64 record = s.x[0];
    const u64 timing = g_mem->Read<u64>(g_main + kTimingPtr);
    if (!timing || !g_mem->IsValid(timing, 0x68) || !g_mem->IsValid(record, 0x100))
        return RunOriginal(s, g_hook);
    const u32 interval = g_mem->Read<u32>(timing + 0x60);
    const float dt = g_mem->Read<float>(timing + 0x18);
    if (interval < 1 || !(dt > 0.0f))
        return RunOriginal(s, g_hook);
    u32 step;
    {
        std::lock_guard lk(g_mutex);
        float& acc = g_ticks[record];
        acc += std::min(dt * 60.0f, float(interval));
        step = u32(std::floor(acc + 1e-3f));
        acc = std::max(0.0f, acc - float(step));
    }
    if (step >= 1) {
        s.x[2] = step;
        RunOriginal(s, g_hook);
        if (g_trace)
            Trace(record);
        return;
    }
    // no whole tick this frame: run with step 1, then take the step back from the buttons already held
    u8 before[kButtons];
    g_mem->ReadBlock(record + 0x34, before, sizeof(before));
    s.x[2] = 1;
    RunOriginal(s, g_hook);
    const u32 type = g_mem->Read<u32>(record + 0xf8);
    if (type >= 4)
        return;
    u32 repeat = g_mem->Read<u32>(record + 0xc);
    const u32 pressed = g_mem->Read<u32>(record + 8);
    u8 after[kButtons];
    g_mem->ReadBlock(record + 0x34, after, sizeof(after));
    for (u32 i = 0; i < kButtons; i++) {
        if (before[i] == 0 || after[i] == 0) // not held before (a fresh press) or released now
            continue;
        const u32 mask = g_mem->Read<u32>(g_main + kMasks + type * 0x80 + i * 8);
        after[i] = before[i];
        const u64 total = record + 0x44 + 4 * i;
        g_mem->Write<u32>(total, g_mem->Read<u32>(total) - 1);
        if (!(pressed & mask))
            repeat &= ~mask;
    }
    g_mem->WriteBlock(record + 0x34, after, sizeof(after));
    g_mem->Write<u32>(record + 0xc, repeat);
    if (g_trace)
        Trace(record);
}

} // namespace

void Install(core::Memory& mem, kernel::Kernel& kernel, u64 main_base, const core::Config& cfg) {
    if (cfg.fps_target <= 60)
        return;
    // the build these offsets come from: the builder's first word, the repeat compares (20, 16), the reset store
    static constexpr std::pair<u64, u32> kExpect[] = {
        {kBuildRecord, 0xd10283ff}, {0x177308, 0x7100517f}, {0x17735c, 0x7100417f}, {0x177324, 0x3900d15b}};
    for (auto [off, word] : kExpect)
        if (!mem.IsValid(main_base + off, 4) || mem.Read<u32>(main_base + off) != word) {
            LOG_WARN("Menu", "unknown game build (main+{:#x}): menu auto-repeat not adjusted", off);
            return;
        }
    g_mem = &mem;
    g_main = main_base;
    g_hook.pc = main_base + kBuildRecord;
    g_hook.host = BuildRecord;
    g_hook.direct = true; // called with BL
    kernel.Cpus().Override(g_hook);
    LOG_INFO("Menu", "menu auto-repeat follows the frame time ({} fps target)", cfg.fps_target);
}

} // namespace hle::menu::repeat
