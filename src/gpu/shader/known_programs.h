// What the game data cannot tell about its shaders, learned from play and shipped with the port so the shader
// build step (mua3 --build-shaders) can compile everything before the first launch. Only hashes, file offsets and
// launch parameters: the programs themselves are read from the player's own install.
#pragma once
#include <span>

#include "common/common.h"

namespace gpu::shader {

// A program the harvest of romfs/SHADER does not find: every compute program (the game copies them to GPU memory from
// its main module, file 0, or from a romfs file, and launches them with a workgroup shape that is part of the cache
// key) and the vertex/pixel programs the harvest misses (stage 0/1, block and cbuf fields unused). Compute constant
// buffer 1 (BRX jump tables) is a main-module range when cb_size != 0; cb_size == kUnknownCbuf: never recorded (only
// programs without BRX are built then).
struct KnownProgram {
    u64 hash;          // ShaderCache key (program + shape)
    u8 stage;          // 0 vertex, 1 pixel, 2 compute (ShaderCache stage index)
    u16 file;          // 0 = main module image, else KnownFiles()[file] (path under romfs/)
    u32 offset, size;  // program bytes (compute: no SPH, the cache puts a zeroed one in front)
    u32 block[3];
    u32 shared_bytes;
    u32 cb_offset, cb_size;
    u64 cb_hash;       // Hash64 of constant buffer 1
};
constexpr u32 kUnknownCbuf = 0xFFFFFFFF;

// Integer variants the game is known to draw with: the base program hash and ShaderCache::VariantKey's fields.
struct KnownVariant {
    u64 base;
    u32 key[6];
};

std::span<const char* const> KnownFiles();
std::span<const KnownProgram> KnownPrograms();
std::span<const KnownVariant> KnownVariants();

} // namespace gpu::shader
