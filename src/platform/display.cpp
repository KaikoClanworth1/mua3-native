#include <d3d11.h>
#include <dxgi1_2.h>

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_sdl3.h>

#include "platform/platform.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

namespace platform {

template <typename T>
static void SafeRelease(T*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

bool Display::Init(const core::Config& cfg, const char* title) {
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | SDL_INIT_AUDIO)) {
        LOG_ERROR("Display", "SDL_Init failed: {}", SDL_GetError());
        return false;
    }
    SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    window_ = SDL_CreateWindow(title, cfg.width, cfg.height, flags);
    if (!window_) {
        LOG_ERROR("Display", "SDL_CreateWindow failed: {}", SDL_GetError());
        return false;
    }
    HWND hwnd = (HWND)SDL_GetPointerProperty(SDL_GetWindowProperties(window_), SDL_PROP_WINDOW_WIN32_HWND_POINTER,
                                             nullptr);
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    UINT dflags = 0;
    if (std::getenv("MUA3_D3D_DEBUG"))
        dflags |= D3D11_CREATE_DEVICE_DEBUG;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, dflags, levels, 2,
                                               D3D11_SDK_VERSION, &sd, &swapchain_, &device_, nullptr, &ctx_);
    if (FAILED(hr) && (dflags & D3D11_CREATE_DEVICE_DEBUG))
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2,
                                           D3D11_SDK_VERSION, &sd, &swapchain_, &device_, nullptr, &ctx_);
    if (FAILED(hr)) {
        LOG_ERROR("Display", "D3D11CreateDeviceAndSwapChain failed: {:#x}", u32(hr));
        return false;
    }
    {
        IDXGIDevice* dxgi = nullptr;
        IDXGIAdapter* adapter = nullptr;
        DXGI_ADAPTER_DESC desc{};
        if (SUCCEEDED(device_->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgi)) &&
            SUCCEEDED(dxgi->GetAdapter(&adapter)) && SUCCEEDED(adapter->GetDesc(&desc))) {
            char name[128]{};
            WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof(name), nullptr, nullptr);
            LOG_INFO("Display", "D3D11 on {}", name);
        }
        SafeRelease(adapter);
        SafeRelease(dxgi);
    }
    CreateBackbuffer();
    vsync_ = cfg.vsync;
    {
        ID3D10Multithread* mt = nullptr;
        if (SUCCEEDED(ctx_->QueryInterface(__uuidof(ID3D10Multithread), (void**)&mt))) {
            mt->SetMultithreadProtected(TRUE);
            mt->Release();
        }
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::StyleColorsDark();
    ImGui_ImplSDL3_InitForD3D(window_);
    ImGui_ImplDX11_Init(device_, ctx_);
    if (cfg.fullscreen)
        ToggleFullscreen();
    return true;
}

void Display::CreateBackbuffer() {
    ID3D11Texture2D* bb = nullptr;
    swapchain_->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb);
    device_->CreateRenderTargetView(bb, nullptr, &rtv_);
    SafeRelease(bb);
}

void Display::Shutdown() {
    if (ImGui::GetCurrentContext()) {
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
    }
    SafeRelease(frame_srv_);
    SafeRelease(frame_tex_);
    SafeRelease(rtv_);
    SafeRelease(swapchain_);
    SafeRelease(ctx_);
    SafeRelease(device_);
    if (window_)
        SDL_DestroyWindow(window_);
    window_ = nullptr;
    SDL_Quit();
}

void Display::ToggleFullscreen() {
    fullscreen_ = !fullscreen_;
    SDL_SetWindowFullscreen(window_, fullscreen_);
}

bool Display::PumpEvents() {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        ImGui_ImplSDL3_ProcessEvent(&e);
        switch (e.type) {
        case SDL_EVENT_QUIT:
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED: return false;
        case SDL_EVENT_WINDOW_FOCUS_GAINED: focused_ = true; break;
        case SDL_EVENT_WINDOW_FOCUS_LOST: focused_ = false; break;
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
            SafeRelease(rtv_);
            swapchain_->ResizeBuffers(0, 0, 0, DXGI_FORMAT_UNKNOWN, DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH);
            CreateBackbuffer();
            break;
        case SDL_EVENT_KEY_DOWN:
            if ((e.key.key == SDLK_RETURN && (e.key.mod & SDL_KMOD_ALT)) || e.key.key == SDLK_F11)
                ToggleFullscreen();
            break;
        default: break;
        }
    }
    return true;
}

void Display::UploadFrame(const u8* rgba, u32 width, u32 height, u32 pitch) {
    std::lock_guard lk(frame_mutex_);
    pending_frame_.resize(size_t(width) * height * 4);
    for (u32 y = 0; y < height; y++)
        std::memcpy(&pending_frame_[size_t(y) * width * 4], rgba + size_t(y) * pitch, size_t(width) * 4);
    pending_w_ = width;
    pending_h_ = height;
    frame_dirty_ = true;
}

void Display::BeginFrame() {
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    {
        std::lock_guard lk(frame_mutex_);
        if (frame_dirty_) {
            if (pending_w_ != frame_w_ || pending_h_ != frame_h_) {
                SafeRelease(frame_srv_);
                SafeRelease(frame_tex_);
                D3D11_TEXTURE2D_DESC td{};
                td.Width = pending_w_;
                td.Height = pending_h_;
                td.MipLevels = 1;
                td.ArraySize = 1;
                td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                td.SampleDesc.Count = 1;
                td.Usage = D3D11_USAGE_DEFAULT;
                td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                device_->CreateTexture2D(&td, nullptr, &frame_tex_);
                device_->CreateShaderResourceView(frame_tex_, nullptr, &frame_srv_);
                frame_w_ = pending_w_;
                frame_h_ = pending_h_;
            }
            ctx_->UpdateSubresource(frame_tex_, 0, nullptr, pending_frame_.data(), frame_w_ * 4, 0);
            frame_dirty_ = false;
        }
    }
    DrawFrameTexture();
    if (draw_overlay)
        draw_overlay();
}

void Display::DrawFrameTexture() {
    ID3D11ShaderResourceView* srv = frame_srv_;
    u32 fw = frame_w_, fh = frame_h_;
    if (frame_source) {
        u32 w = 0, h = 0;
        if (auto* s = frame_source(w, h); s && w && h)
            srv = s, fw = w, fh = h;
    }
    if (!srv || !fw || !fh)
        return;
    const ImVec2 ds = ImGui::GetIO().DisplaySize;
    const float sx = ds.x / fw, sy = ds.y / fh;
    const float s = std::min(sx, sy);
    const float w = fw * s, h = fh * s;
    const ImVec2 p0((ds.x - w) * 0.5f, (ds.y - h) * 0.5f);
    ImGui::GetBackgroundDrawList()->AddImage((ImTextureID)srv, p0, ImVec2(p0.x + w, p0.y + h));
}

void Display::EndFrame() {
    std::unique_lock<std::recursive_mutex> lk;
    if (context_mutex)
        lk = std::unique_lock(*context_mutex);
    ImGui::Render();
    const float clear[4] = {0, 0, 0, 1};
    ctx_->OMSetRenderTargets(1, &rtv_, nullptr);
    ctx_->ClearRenderTargetView(rtv_, clear);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    if (context_epoch)
        (*context_epoch)++;
    // Don't hold the context lock while Present waits for vblank: the GPU thread needs the context.
    if (lk.owns_lock())
        lk.unlock();
    // Never block inside Present: it holds the D3D device lock the GPU thread needs. With flip-discard and
    // sync interval 0 DWM still composes without tearing; the UI loop paces itself (PaceFrame).
    swapchain_->Present(0, 0);
}

void Display::PaceFrame() {
    using namespace std::chrono;
    static thread_local HANDLE timer =
        CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    const auto period = duration_cast<steady_clock::duration>(duration<double>(1.0 / 60.0));
    auto now = steady_clock::now();
    if (next_frame_ < now - period)
        next_frame_ = now;
    next_frame_ += period;
    const auto wait = next_frame_ - now;
    if (wait > microseconds(500) && timer) {
        LARGE_INTEGER due;
        due.QuadPart = -LONGLONG(duration_cast<nanoseconds>(wait).count() / 100);
        SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
        WaitForSingleObject(timer, INFINITE);
    }
}

} // namespace platform
