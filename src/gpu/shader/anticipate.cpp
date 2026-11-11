#include "gpu/shader/anticipate.h"

#include <mutex>
#include <utility>
#include <unordered_set>

namespace gpu::shader {
namespace {
std::mutex g_mutex;
std::vector<u64> g_queue;
std::unordered_set<u64> g_seen; // queued since the last take (the cache skips programs it already handled)
} // namespace

void AnticipatePrograms(std::span<const u64> data) {
    std::lock_guard lk(g_mutex);
    for (size_t i = 0; i < data.size(); i += 2) {
        if (!data[i])
            continue;
        // the Maxwell program header follows NVN's 0x30-byte blob header (as the encoder binds it)
        const u64 va = (data[i] + 0x30) & 0xFFFFFFFFFFull;
        if (g_queue.size() < (1u << 14) && g_seen.insert(va).second)
            g_queue.push_back(va);
    }
}

std::vector<u64> TakeAnticipated() {
    std::lock_guard lk(g_mutex);
    g_seen.clear();
    return std::exchange(g_queue, {});
}

} // namespace gpu::shader
