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
#ifndef NDEBUG
    dflags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
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
    if (!frame_srv_)
        return;
    const ImVec2 ds = ImGui::GetIO().DisplaySize;
    const float sx = ds.x / frame_w_, sy = ds.y / frame_h_;
    const float s = std::min(sx, sy);
    const float w = frame_w_ * s, h = frame_h_ * s;
    const ImVec2 p0((ds.x - w) * 0.5f, (ds.y - h) * 0.5f);
    ImGui::GetBackgroundDrawList()->AddImage((ImTextureID)frame_srv_, p0, ImVec2(p0.x + w, p0.y + h));
}

void Display::EndFrame() {
    ImGui::Render();
    const float clear[4] = {0, 0, 0, 1};
    ctx_->OMSetRenderTargets(1, &rtv_, nullptr);
    ctx_->ClearRenderTargetView(rtv_, clear);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    swapchain_->Present(vsync_ ? 1 : 0, 0);
}

} // namespace platform
