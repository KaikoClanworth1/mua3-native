#pragma once
#include <span>
#include <string>

#include "common/common.h"

namespace core {

class Memory;
struct Config;

// Game-specific code patches for known builds (verified by build id and original instruction words), applied
// to a module image after it is written to guest memory and before it runs.
void ApplyGamePatches(Memory& mem, const std::string& module, u64 base, std::span<const u8> build_id, const Config& cfg);

} // namespace core
