// PC SETTINGS > CAMERA SPEED.
//
// The player camera's update (main+0x339d40, called with BL) turns the camera by
//   yaw   += speed_h * (stick_x - deadzone)^2 * dt   (speed_h: float at P + 0x7e0 or P + 0x7f0)
//   pitch += speed_v * (stick_y - deadzone)^2 * dt   (speed_v: float at P + 0x7d0)
// with P = [[[[main+0x2187cc8]] + 0x2920] + 0x200] (the camera parameters; the deadzone is at P + 0x800 / 0x810).
// The turn rate is linear in the speed values, so the update is wrapped: for its duration the three speed floats
// are multiplied by the CAMERA SPEED percentage and then put back. The rate is that percentage of the original at
// every tilt, and nothing else (the stick, menus, the CAMERA tab's own settings) changes. At 100% the wrapper only
// calls the original.
//
// MOUSE CAMERA: the camera object (x0 of the update) keeps its yaw at +0x220 (copies at +0x270 and +0x2c0) and its
// pitch at +0x228 (copies +0x278, +0x2c8, +0x318), radians; the update adds the stick's turn to the pitch at +0x2c8
// and clamps it to [+0x1f0, +0x1f4] (main+0x33a42c: both 0 where the game allows no pitch, e.g. ST010's start).
// Mouse counts gathered since the last update are added before it runs: yaw -= dx * k (right turns right, as the
// stick), pitch += dy * k within the game's own limits (none where it allows none), k = 0.0025 rad per count at
// 100% sensitivity.
#include "core/hle/camera_speed.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <mutex>

#include "core/hle/menu_guest.h"
#include "core/kernel/kernel.h"
#include "core/memory.h"

namespace hle::camera_speed {
namespace {

constexpr u64 kCameraUpdate = 0x339d40;
constexpr u64 kParamsGot = 0x2187cc8; // GOT entry of the global whose object holds the camera parameters
constexpr u64 kSpeeds[] = {0x7d0, 0x7e0, 0x7f0};

cpu::FunctionOverride g_hook;
core::Memory* g_mem = nullptr;
u64 g_main = 0;
std::atomic<int> g_percent{100};
// mouse camera
constexpr float kRadPerCount = 0.0025f; // at 100%
constexpr float kPi = 3.14159265f;
std::atomic<bool> g_mouse_on{true}, g_invert{false};
std::atomic<int> g_mouse_pct{100};
std::mutex g_mouse_mutex;
float g_dx = 0, g_dy = 0; // counts not yet applied
std::atomic<s64> g_last_update{0}; // steady_clock ticks of the last camera update
const bool g_trace = std::getenv("MUA3_TRACE_CAMERA") != nullptr; // test aid: log the yaw every 50 ms

void Trace(u64 camera) {
    static const auto t0 = std::chrono::steady_clock::now();
    static double last = -1.0;
    const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    if (t - last < 0.05)
        return;
    last = t;
    LOG_INFO("Camera", "camera yaw {:.5f} at {:.3f} s ({:#x})", g_mem->Read<float>(camera + 0x220), t, camera);
}

u64 Params() {
    u64 p = g_mem->Read<u64>(g_main + kParamsGot);
    for (u64 off : {u64(0), u64(0x2920), u64(0x200)}) {
        if (!p || !g_mem->IsValid(p + off, 8))
            return 0;
        p = g_mem->Read<u64>(p + off);
    }
    return p && g_mem->IsValid(p + 0x7d0, 0x30) ? p : 0;
}

float Wrap(float a) {
    while (a > kPi)
        a -= 2 * kPi;
    while (a < -kPi)
        a += 2 * kPi;
    return a;
}

// The mouse's counts since the last update, into the camera's yaw and pitch (all their copies)
void ApplyMouse(u64 camera) {
    float dx, dy;
    {
        std::lock_guard lk(g_mouse_mutex);
        dx = g_dx, dy = g_dy;
        g_dx = g_dy = 0;
    }
    if ((dx == 0 && dy == 0) || !g_mouse_on.load(std::memory_order_relaxed) || !g_mem->IsValid(camera + 0x1f0, 0x130))
        return;
    const float k = kRadPerCount * float(g_mouse_pct.load(std::memory_order_relaxed)) / 100.0f;
    const float yaw = Wrap(g_mem->Read<float>(camera + 0x220) - dx * k);
    for (u64 o : {0x220, 0x270, 0x2c0})
        g_mem->Write<float>(camera + o, yaw);
    const float lo = g_mem->Read<float>(camera + 0x1f0), hi = g_mem->Read<float>(camera + 0x1f4);
    const bool pitch_free = hi > lo;
    float pitch = g_mem->Read<float>(camera + 0x2c8);
    if (pitch_free && dy != 0) {
        pitch = std::clamp(pitch + (g_invert.load(std::memory_order_relaxed) ? -dy : dy) * k, lo, hi);
        for (u64 o : {0x228, 0x278, 0x2c8, 0x318})
            g_mem->Write<float>(camera + o, pitch);
    }
    static float sum_x = 0, sum_y = 0;
    static auto last = std::chrono::steady_clock::time_point{};
    sum_x += dx, sum_y += dy;
    if (std::chrono::steady_clock::now() - last >= std::chrono::seconds(1)) {
        last = std::chrono::steady_clock::now();
        LOG_INFO("Camera", "mouse camera: {:.0f} / {:.0f} counts -> yaw {:+.4f}, pitch {:+.4f} rad ({:.5f} rad/count; yaw "
                           "now {:.4f}, pitch {:.4f}{})",
                 sum_x, sum_y, -sum_x * k, pitch_free ? sum_y * k : 0.0f, k, yaw, pitch,
                 pitch_free ? fmt::format(" in [{:.3f}, {:.3f}]", lo, hi) : std::string(", pitch locked here"));
        sum_x = sum_y = 0;
    }
}

void CameraUpdate(recomp::State& s) {
    const bool was_active = CameraActive();
    g_last_update.store(std::chrono::steady_clock::now().time_since_epoch().count(), std::memory_order_relaxed);
    if (!was_active) { // counts from while the camera was not running (menu, pause, cut-scene) are dropped
        std::lock_guard lk(g_mouse_mutex);
        if (g_dx != 0 || g_dy != 0)
            LOG_INFO("Camera", "mouse camera: {:.0f} / {:.0f} counts from while the camera was stopped dropped", g_dx, g_dy);
        g_dx = g_dy = 0;
    }
    ApplyMouse(s.x[0]);
    const int percent = g_percent.load(std::memory_order_relaxed);
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true)) // test aid: the camera object (its yaw, radians, is at +0x220)
        LOG_INFO("Camera", "camera object {:#x} at {}%", s.x[0], percent);
    const u64 camera = s.x[0];
    const u64 p = percent == 100 ? 0 : Params();
    if (!p) {
        menu::RunOriginal(s, g_hook);
        if (g_trace)
            Trace(camera);
        return;
    }
    const float f = float(percent) / 100.0f;
    float saved[std::size(kSpeeds)];
    for (size_t i = 0; i < std::size(kSpeeds); i++) {
        saved[i] = g_mem->Read<float>(p + kSpeeds[i]);
        g_mem->Write<float>(p + kSpeeds[i], saved[i] * f);
    }
    static std::atomic<bool> logged_speeds{false};
    if (!logged_speeds.exchange(true))
        LOG_INFO("Camera", "camera speeds {} {} {} x {}%", saved[0], saved[1], saved[2], percent);
    menu::RunOriginal(s, g_hook);
    for (size_t i = 0; i < std::size(kSpeeds); i++)
        g_mem->Write<float>(p + kSpeeds[i], saved[i]);
    if (g_trace)
        Trace(camera);
}

} // namespace

void Set(int percent) {
    g_percent.store(std::clamp(percent, 10, 200), std::memory_order_relaxed);
}

void SetMouse(bool on, int sensitivity, bool invert_y) {
    g_mouse_on = on;
    g_mouse_pct = std::clamp(sensitivity, 10, 500);
    g_invert = invert_y;
}

void AddMouse(float dx, float dy) {
    std::lock_guard lk(g_mouse_mutex);
    g_dx += dx;
    g_dy += dy;
}

bool CameraActive() {
    const s64 last = g_last_update.load(std::memory_order_relaxed);
    return last && std::chrono::steady_clock::now().time_since_epoch().count() - last <
                       std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::milliseconds(150)).count();
}

void Install(core::Memory& mem, kernel::Kernel& kernel, u64 main_base, int percent) {
    Set(percent);
    // the build these offsets come from: the update's first word, the parameter loads and the speed reads
    static constexpr std::pair<u64, u32> kExpect[] = {{kCameraUpdate, 0x6db63bef}, {0x339fc8, 0xf9549129},
                                                      {0x339fcc, 0xf9410129},      {0x339fe8, 0xbc686940},
                                                      {0x339ffc, 0xbd47d120},      {0x33a488, 0xbd4047e3},
                                                      {0x33a41c, 0xbd4043e1}};
    for (auto [off, word] : kExpect)
        if (!mem.IsValid(main_base + off, 4) || mem.Read<u32>(main_base + off) != word) {
            LOG_WARN("Camera", "unknown game build (main+{:#x}): CAMERA SPEED has no effect", off);
            return;
        }
    g_mem = &mem;
    g_main = main_base;
    g_hook.pc = main_base + kCameraUpdate;
    g_hook.host = CameraUpdate;
    g_hook.direct = true; // called with BL
    kernel.Cpus().Override(g_hook);
    LOG_INFO("Camera", "camera speed scales the game's turn speed ({}%)", percent);
}

} // namespace hle::camera_speed
