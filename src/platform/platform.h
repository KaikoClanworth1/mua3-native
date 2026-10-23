#pragma once
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
struct ID3D11ShaderResourceView;

#include "common/common.h"
#include "core/config.h"

struct SDL_Window;
struct ID3D11Device;
struct ID3D11DeviceContext;

namespace platform {

// Window + D3D11 device + swapchain. Owns the ImGui context used for the in-game overlay.
class Display {
public:
    bool Init(const core::Config& cfg, const char* title);
    void Shutdown();
    // Returns false when the window was closed.
    bool PumpEvents();
    void BeginFrame();
    void EndFrame(); // draws the overlay and presents
    void PaceFrame(); // sleeps until the next UI frame (60 Hz)

    // Latest guest frame as RGBA8 (linear layout) if available.
    void UploadFrame(const u8* rgba, u32 width, u32 height, u32 pitch);

    SDL_Window* Window() const { return window_; }
    ID3D11Device* Device() const { return device_; }
    ID3D11DeviceContext* Context() const { return ctx_; }
    bool Focused() const { return focused_; }
    void ToggleFullscreen();

    std::function<void()> draw_overlay; // ImGui calls
    // Supplies the latest rendered game frame (called with context_mutex held).
    std::function<ID3D11ShaderResourceView*(u32& w, u32& h)> frame_source;
    std::recursive_mutex* context_mutex = nullptr;
    std::atomic<u64>* context_epoch = nullptr; // bumped whenever the UI used the shared context (state caches)

private:
    void CreateBackbuffer();
    void DrawFrameTexture();

    SDL_Window* window_ = nullptr;
    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* ctx_ = nullptr;
    struct IDXGISwapChain* swapchain_ = nullptr;
    struct ID3D11RenderTargetView* rtv_ = nullptr;
    struct ID3D11Texture2D* frame_tex_ = nullptr;
    struct ID3D11ShaderResourceView* frame_srv_ = nullptr;
    u32 frame_w_ = 0, frame_h_ = 0;
    bool vsync_ = true, focused_ = true, fullscreen_ = false;
    std::chrono::steady_clock::time_point next_frame_{};
    std::mutex frame_mutex_;
    std::vector<u8> pending_frame_;
    u32 pending_w_ = 0, pending_h_ = 0;
    bool frame_dirty_ = false;
};

// Polls the selected backend and feeds HID.
class Input {
public:
    bool Init(core::InputBackend backend);
    void Update(bool window_focused);
    std::string DeviceName() const { return device_name_; }

private:
    core::InputBackend backend_{};
    std::string device_name_ = "Keyboard";
    struct SDL_Gamepad* pad_ = nullptr;
};

// Host audio output. The HLE audio renderer pushes interleaved stereo s16 48 kHz samples.
class AudioOut {
public:
    bool Init(core::AudioBackend backend, int volume);
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
