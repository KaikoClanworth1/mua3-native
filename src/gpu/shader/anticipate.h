#pragma once
// Programs the game set up (nvnProgramSetShaders; MUA3 sets up all of them at boot): the renderer's shader cache looks
// each one up, so a program it has never seen (some are built at runtime, not in the game files) compiles before its
// first draw instead of making that draw wait.
#include <span>
#include <vector>

#include "common/common.h"

namespace gpu::shader {

// Any thread: `data` is nvnProgramSetShaders' {code address, control pointer} pairs.
void AnticipatePrograms(std::span<const u64> data);

// The shader cache (GPU thread): the GPU addresses of the program headers queued since the last call (each once per
// call; a program set up again later is queued again).
std::vector<u64> TakeAnticipated();

} // namespace gpu::shader
