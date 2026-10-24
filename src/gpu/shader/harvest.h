#pragma once
#include "common/common.h"

namespace gpu {

// FNV-1a style hash over 8-byte words; keys the shader cache, its dump manifest and the bytecode cache.
u64 Hash64(const void* data, size_t size);

namespace shader {

// Finds every vertex/pixel program shipped in the game data under `root` (KTGL keeps them raw, header included,
// inside SHADER/*.spkg|*.kps) and writes each one to `out` as <hash>.<vs|ps>.bin, the shader cache's manifest
// format, so they can all be compiled before they are first drawn. Returns the number of new programs.
size_t HarvestPrograms(const fs::path& root, const fs::path& out);

} // namespace shader
} // namespace gpu
