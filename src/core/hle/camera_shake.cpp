// Camera shake at frame rates above 60.
//
// The camera manager's per-frame update (main+0x2b0290) advances the four camera-shake controllers
// (main+0x3556a0, objects at manager +0x1eaa0 + 0xd0 * n) by the present interval read as an integer from KTGL's
// timing block (main+0x2188a00 -> +0x60): 2 at 30 fps, 1 at 60, i.e. in 1/60 s ticks. With the frame rate unlocked
// above 60 the interval stays 1, so shakes ran fps/60 times too fast (2.4x at 144). The rest of the camera already
// uses the frame time (+0x18).
//
// The step is a function argument (s0), but the call sites are BLs inside the manager whose return addresses are not
// native entry points, so the shake function itself cannot be overridden. Instead the manager is: before it runs,
// each controller is stepped once by (dt * 60 - interval), so with the manager's own step the net advance per frame
// is dt * 60 ticks. The step is linear in s0 (phase += s0 * rate, timer += s0), so the two calls add up exactly.
// Installed only when fps_target is above 60; at 30/60 nothing changes.
#include "core/hle/camera_shake.h"

#include <algorithm>
#include <cstring>

#include "core/config.h"
#include "core/hle/menu_guest.h"
#include "core/kernel/kernel.h"
#include "core/memory.h"

namespace hle::camera {
namespace {

constexpr u64 kCameraUpdate = 0x2b0290; // camera manager per-frame update (x0 = manager)
constexpr u64 kShakeStep = 0x3556a0;    // shake controller step (x0 = controller, s0 = ticks)
constexpr u64 kTimingPtr = 0x2188a00;   // KTGL timing block pointer: +0x18 frame time (s), +0x60 present interval
constexpr u32 kShakeObjects[4] = {0x1eaa0, 0x1eb70, 0x1ec40, 0x1ed10};

core::Memory* g_mem = nullptr;
u64 g_main = 0;
cpu::FunctionOverride g_hook;

void CameraUpdate(recomp::State& s) {
    const u64 timing = g_mem->Read<u64>(g_main + kTimingPtr);
    if (timing && g_mem->IsValid(timing, 0x68)) {
        const u32 interval = g_mem->Read<u32>(timing + 0x60);
        const float dt = g_mem->Read<float>(timing + 0x18);
        const float ticks = std::min(dt * 60.0f, float(interval));
        if (interval >= 1 && dt > 0.0f && ticks < float(interval) - 1e-4f) {
            const float extra = ticks - float(interval); // negative: takes back the part of the manager's step
            u32 bits;
            std::memcpy(&bits, &extra, 4);
            const recomp::V128 v0 = s.v[0];
            const u64 manager = s.x[0];
            for (u32 off : kShakeObjects) {
                s.v[0] = recomp::Make(bits, 0);
                menu::GuestCall(s, g_main + kShakeStep, {manager + off});
            }
            s.v[0] = v0;
        }
    }
    menu::RunOriginal(s, g_hook);
}

} // namespace

void Install(core::Memory& mem, kernel::Kernel& kernel, u64 main_base, const core::Config& cfg) {
    if (cfg.fps_target <= 60)
        return;
    // the build these offsets come from: the first words of both functions and the interval load in the manager
    static constexpr std::pair<u64, u32> kExpect[] = {
        {kCameraUpdate, 0x6db733ed}, {kShakeStep, 0x6db83bef}, {0x2b0350, 0xbd406100}};
    for (auto [off, word] : kExpect)
        if (!mem.IsValid(main_base + off, 4) || mem.Read<u32>(main_base + off) != word) {
            LOG_WARN("Camera", "unknown game build (main+{:#x}): camera shake timing not adjusted", off);
            return;
        }
    g_mem = &mem;
    g_main = main_base;
    g_hook.pc = main_base + kCameraUpdate;
    g_hook.host = CameraUpdate;
    g_hook.direct = true; // called with BL
    kernel.Cpus().Override(g_hook);
    LOG_INFO("Camera", "camera shake follows the frame time ({} fps target)", cfg.fps_target);
}

} // namespace hle::camera
