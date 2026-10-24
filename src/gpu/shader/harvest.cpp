#include "gpu/shader/harvest.h"

#include <cstring>

#include "gpu/shader/translator.h"

namespace gpu {

u64 Hash64(const void* data, size_t size) {
    // FNV-1a 64 over 8-byte words (fast enough for shader programs).
    const u8* p = static_cast<const u8*>(data);
    u64 h = 0xcbf29ce484222325ull;
    size_t i = 0;
    for (; i + 8 <= size; i += 8) {
        u64 w;
        std::memcpy(&w, p + i, 8);
        h ^= w;
        h *= 0x100000001b3ull;
        h ^= h >> 29;
    }
    for (; i < size; i++) {
        h ^= p[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

namespace shader {

size_t HarvestPrograms(const fs::path& root, const fs::path& out) {
    std::error_code ec;
    fs::create_directories(out, ec);
    size_t added = 0;
    for (const auto& e : fs::recursive_directory_iterator(root, ec)) {
        if (!e.is_regular_file())
            continue;
        const auto data = common::ReadFile(e.path());
        for (size_t off = 0; off + 0x58 <= data.size(); off += 4) { // packages only keep 4-byte alignment
            u32 w0;
            std::memcpy(&w0, &data[off], 4);
            // SPH word 0: sph type (bits 0-4: 1 vertex, 2 pixel), version 3 (bits 5-9), shader type (bits 10-13:
            // 1 vertex B, 5 pixel) and SASS version 3 (bits 17-20). Package metadata can look like a header, so
            // check all four; candidates can also overlap real programs, so every offset is tried.
            const u32 type = w0 & 0x1F, version = (w0 >> 5) & 0x1F, stype = (w0 >> 10) & 0xF, sass = (w0 >> 17) & 0xF;
            if (!((type == 1 && stype == 1) || (type == 2 && stype == 5)) || version != 3 || sass != 3)
                continue;
            const size_t size = ProgramSize(&data[off], data.size() - off);
            if (size <= 0x58)
                continue;
            const Stage stage = type == 1 ? Stage::Vertex : Stage::Fragment;
            const u64 hash = Hash64(&data[off], size) ^ (u64(stage) << 60);
            const fs::path p = out / fmt::format("{:016x}.{}.bin", hash, type == 1 ? "vs" : "ps");
            if (fs::exists(p) || !Translate(stage, &data[off], size).ok)
                continue;
            common::WriteFile(p, std::span(&data[off], size));
            added++;
        }
    }
    return added;
}

} // namespace shader
} // namespace gpu
