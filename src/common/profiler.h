#pragma once
#include "common/common.h"

namespace common {

// Poor man's sampling profiler: periodically suspends `thread_handle`, records the innermost
// symbolized frames, and logs the hottest functions after `seconds`. Runs on its own thread.
void StartSamplingProfiler(void* thread_handle, const char* name, int seconds, int start_delay_seconds);

} // namespace common
