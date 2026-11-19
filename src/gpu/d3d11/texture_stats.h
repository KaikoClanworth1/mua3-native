#pragma once
// Texture upload counters since boot, read by the 5 s memory line in main.cpp (GitHub #8: a host memory leak on AMD
// while textures the CPU rewrites, the game's glyph caches, are uploaded again).
#include <atomic>

#include "common/common.h"

namespace gpu {

struct TextureStats {
    std::atomic<u64> uploads{0}, upload_bytes{0};     // new textures
    std::atomic<u64> reuploads{0}, reupload_bytes{0}; // CPU-rewritten textures uploaded again (in place or re-created)
    std::atomic<u64> partial{0};                      // (of those) only the rewritten part
    std::atomic<u64> staged{0};                       // (of those) through a staging texture (MUA3_TEX_STAGING)
    std::atomic<u64> unchanged{0};                    // pages written, contents as uploaded: nothing uploaded
};
inline TextureStats g_texture_stats;

} // namespace gpu
