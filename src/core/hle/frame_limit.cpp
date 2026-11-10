#include "core/hle/frame_limit.h"

#include <chrono>
#include <thread>
#include <windows.h>

#include "core/system.h"

namespace hle {

void LimitFrameRate() {
    const core::Config& cfg = core::Sys().Cfg();
    const int limit = cfg.fps_limit;
    if (limit <= 0 || limit >= cfg.fps_target)
        return;
    using clock = std::chrono::steady_clock;
    static clock::time_point deadline{}; // the game presents from one thread
    const auto period = std::chrono::duration_cast<clock::duration>(std::chrono::duration<double>(1.0 / limit));
    auto now = clock::now();
    if (deadline < now - 2 * period) // first frame, or after a stall (loading): start a new grid
        deadline = now;
    deadline += period;
    while ((now = clock::now()) < deadline) {
        const auto left = deadline - now;
        if (left > std::chrono::milliseconds(2))
            std::this_thread::sleep_until(deadline - std::chrono::milliseconds(1));
        else
            SwitchToThread();
    }
}

} // namespace hle
