#pragma once
// Host implementations of NVN API functions. The game resolves every NVN entry point by name through
// nvnDeviceGetProcAddress and calls it through a pointer; the CPU runtime records which driver function each name
// resolves to and, when a host implementation exists, runs that instead of the sdk's NVN driver.
// See docs/native_renderer_plan.md.
#include <atomic>
#include <functional>
#include <string_view>

#include "core/cpu/recomp/recomp_rt.h"

namespace hle::nvn {

// Host implementation for an NVN entry point name, or nullptr if the guest driver still handles it. `driver` is the
// driver's own (recompiled) implementation at guest address `pc`, which observing wrappers call through to. Host functions follow the
// guest calling convention: arguments in s.x / s.v, result in s.x[0], and they return with s.pc = s.x[30] like a
// recompiled function.
recomp::Fn Find(std::string_view name, recomp::Fn driver, recomp::u64 pc);

// Asks the driver's name lookup function (guest `lookup_pc`) for every host-implemented entry point not seen yet
// (some are resolved by code the runtime does not observe) and reports each through `found`.
void ResolveMissing(recomp::State& s, recomp::Fn lookup, recomp::u64 lookup_pc, recomp::u64 device,
                    const std::function<void(const char*, recomp::u64)>& found);

// MUA3_NVN_LEARN: end of the current learning window (steady_clock ms), opened when MUA3_NVN_DUMP writes a frame,
// so the emulated path's resolutions can be joined with that same frame's NVN commands.
extern std::atomic<long long> g_learn_until_ms;

} // namespace hle::nvn
