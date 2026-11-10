// MUA3_TRACE_NATIVE: call logging for recompiled guest functions (see native_trace.cpp).
#pragma once
#include <vector>
#include "core/system.h"

namespace kernel {
class Kernel;
}

namespace hle::trace {
void Install(kernel::Kernel& kernel, const std::vector<core::LoadedModule>& modules);
}
