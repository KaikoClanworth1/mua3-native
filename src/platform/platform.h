#pragma once
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>
struct ID3D11ShaderResourceView;

#include "common/common.h"
#include "core/config.h"

struct SDL_Window;
struct ID3D11Device;
struct ID3D11DeviceContext;
struct _XINPUT_GAMEPAD;
namespace hle::hid {
struct PadInput;
}

namespace platform {

// Window + D3D11 device + swapchain. Owns the ImGui context used for the in-game overlay.
class Display {
public:
    bool Init(const core::Config& cfg, const char* title);
    // Window (and taskbar) icon from an image file: the game's own icon from its installed control data.
    void SetIcon(const fs::path& image);
    void Shutdown();
    // Returns false when the window was closed.
    bool PumpEvents();
    void BeginFrame();
    void EndFrame(); // draws the overlay and presents
    void PaceFrame(); // sleeps until the next UI frame (60 Hz)
    // Height of the output in physical pixels (the desktop in fullscreen), for automatic render scaling.
    u32 PixelHeight() const;

    // Latest guest frame as RGBA8 (linear layout) if available.
    void UploadFrame(const u8* rgba, u32 width, u32 height, u32 pitch);

    SDL_Window* Window() const { return window_; }
    ID3D11Device* Device() const { return device_; }
    ID3D11DeviceContext* Context() const { return ctx_; }
    bool Focused() const { return focused_; }
    void ToggleFullscreen();

    std::function<void()> draw_overlay; // ImGui calls
    // Supplies the latest rendered game frame with a reference the display releases once it drew it (called on the UI
    // thread without context_mutex: the GPU thread may replace the frame at any time).
    std::function<ID3D11ShaderResourceView*(u32& w, u32& h)> frame_source;
    std::recursive_mutex* context_mutex = nullptr;
    std::atomic<u64>* context_epoch = nullptr; // bumped whenever the UI used the shared context (state caches)
    fs::path capture_path; // when set, the next presented frame is saved there as a BMP (then cleared)

    // Device lost (DXGI_ERROR_DEVICE_REMOVED / DEVICE_RESET / DRIVER_INTERNAL_ERROR): flagged by Present, by the
    // renderer (device_lost_check) or by the test hook MUA3_TEST_DEVICE_LOST=<seconds>[,<seconds>...]; the UI
    // thread then calls RecoverDevice: under context_mutex, release_device drops the renderer's objects, the device
    // and swap chain are created again, and restore_device hands the new ones over. Returns false if no device
    // could be created (the caller retries next frame).
    bool DeviceLost();
    bool RecoverDevice();
    u32 DeviceResets() const { return device_resets_; }
    std::function<bool()> device_lost_check;
    std::function<void()> release_device;
    std::function<void(ID3D11Device*, ID3D11DeviceContext*)> restore_device;

private:
    bool CreateDevice();
    void LogRemovedReason(const char* where, long hr);
    bool lost_ = false;
    u32 device_resets_ = 0;
    std::vector<double> test_lost_at_; // MUA3_TEST_DEVICE_LOST: seconds after Init
    std::chrono::steady_clock::time_point init_time_{};
    void CreateBackbuffer();
    static bool TearingSupported();
    u32 SwapChainFlags() const;
    void SaveBackbuffer(const fs::path& path);
    void DrawFrameTexture();

    SDL_Window* window_ = nullptr;
    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* ctx_ = nullptr;
    struct IDXGISwapChain* swapchain_ = nullptr;
    struct ID3D11RenderTargetView* rtv_ = nullptr;
    struct ID3D11Texture2D* frame_tex_ = nullptr;
    struct ID3D11ShaderResourceView* frame_srv_ = nullptr;
    // The view the overlay's draw list samples this UI frame, referenced from BeginFrame until EndFrame drew it: the
    // GPU thread may replace its own between the two (GitHub #12: a released view drawn, a crash in the driver).
    struct ID3D11ShaderResourceView* shown_srv_ = nullptr;
    struct ID3D11BlendState* opaque_blend_ = nullptr; // the game frame is drawn without blending (its alpha is not coverage)
    u32 frame_w_ = 0, frame_h_ = 0;
    const core::Config* live_cfg_ = nullptr; // the caller's config (outlives the display): vsync read per present
    void* frame_wait_ = nullptr;             // the swap chain's frame latency waitable object
    bool tearing_ = false, focused_ = true, fullscreen_ = false;
    u64 mouse_moved_ms_ = 0; // last mouse movement/click: the cursor hides 2 s after it (#15)
    bool cursor_hidden_ = false;
    std::chrono::steady_clock::time_point next_frame_{};
    std::mutex frame_mutex_;
    std::vector<u8> pending_frame_;
    u32 pending_w_ = 0, pending_h_ = 0;
    bool frame_dirty_ = false;
};

// Polls the selected backend and feeds HID.
class Input {
public:
    bool Init(core::InputBackend backend, int camera_speed = 100, int button_layout = 0);
    void Update(bool window_focused);
    void SetCameraSpeed(int percent) { camera_speed_ = percent < 10 ? 10 : percent > 200 ? 200 : percent; }
    void SetButtonLayout(int layout); // core::Config::button_layout
    // Stick deadzones in percent (core::Config::left_deadzone / right_deadzone).
    void SetDeadzones(int left, int right) { left_dz_ = left; right_dz_ = right; }
    std::string DeviceName() const { return device_name_; }
    // Test aid: an Xbox pad holding these XINPUT_GAMEPAD_* buttons (and kSim*Trigger), through the same mapping; returns
    // the game's buttons.
    static constexpr u32 kSimLeftTrigger = 1u << 16, kSimRightTrigger = 1u << 17;
    u64 SimulateXInput(u32 buttons);
    // Test aid: these keys (USB HID usages) and mouse buttons (bit n = button n + 1) held, through the bindings and the
    // KEY BINDINGS capture; returns the game buttons.
    u64 SimulateKeys(const std::vector<u16>& keys, u32 mouse);

private:
    core::InputBackend backend_{};
    int camera_speed_ = 100;
    int button_layout_ = 0;
    int left_dz_ = 10, right_dz_ = 10;
    std::string device_name_ = "Keyboard";
    struct SDL_Gamepad* pad_ = nullptr;
    // The game button each face button (south, east, west, north) went down as, kept until it's released; 0 = up.
    u64 face_[4] = {};
    bool active_[4] = {}; // by hle::prompts::Device: in use last poll
    std::chrono::steady_clock::time_point device_changed_{}; // the prompts' last device change
    bool pad_seen_ = false; // a pad was connected last poll (a pad that appears sets the prompts' device at once)
    void MapXInput(const ::_XINPUT_GAMEPAD& g, hle::hid::PadInput& p);
    u64 Face(int i, bool down, u64 by_layout, u64 by_pos, bool wheel);
    bool KeyboardAndMouse(const bool* keys, u32 mouse, hle::hid::PadInput& p);
};

// Host audio output. The HLE audio renderer pushes interleaved stereo s16 48 kHz samples.
class AudioOut {
public:
    bool Init(core::AudioBackend backend, int volume);
    void SetVolume(int volume); // 0-100, live
    void Shutdown();
    void Push(const s16* samples, size_t frames);
    size_t Queued() const;
    std::string BackendName() const;

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
    core::AudioBackend backend_{};
};

AudioOut& Audio();

} // namespace platform
