#pragma once
// Host implementations of NVN API functions. The game resolves every NVN entry point by name through
// nvnDeviceGetProcAddress and calls it through a pointer; the CPU runtime records which driver function each name
// resolves to and, when a host implementation exists, runs that instead of the sdk's NVN driver.
// See docs/native_renderer_plan.md.
#include <string_view>

#include "core/cpu/recomp/recomp_rt.h"

namespace hle::nvn {

// Host implementation for an NVN entry point name, or nullptr if the guest driver still handles it. Host functions
// follow the guest calling convention: arguments in s.x / s.v, result in s.x[0], and they return with
// s.pc = s.x[30] like a recompiled function.
recomp::Fn Find(std::string_view name);

} // namespace hle::nvn
