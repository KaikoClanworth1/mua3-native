#include <algorithm>
#include <chrono>
#include <ctime>
#include <cstdlib>
#include <d3d11.h>
#include <dxgi1_5.h>

#include <wincodec.h>

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_sdl3.h>

#include "platform/platform.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "windowscodecs.lib")

namespace platform {

template <typename T>
static void SafeRelease(T*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

void Display::SetIcon(const fs::path& image) {
    if (!window_ || !fs::exists(image))
        return;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IWICImagingFactory* wic = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))))
        return;
    IWICBitmapDecoder* dec = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICBitmapScaler* scaler = nullptr;
    IWICFormatConverter* conv = nullptr;
    constexpr UINT kSize = 64; // window icons are drawn small; SDL hands Windows this one bitmap
    if (SUCCEEDED(wic->CreateDecoderFromFilename(image.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &dec)) &&
        SUCCEEDED(dec->GetFrame(0, &frame)) && SUCCEEDED(wic->CreateBitmapScaler(&scaler)) &&
        SUCCEEDED(scaler->Initialize(frame, kSize, kSize, WICBitmapInterpolationModeHighQualityCubic)) &&
        SUCCEEDED(wic->CreateFormatConverter(&conv)) &&
        SUCCEEDED(conv->Initialize(scaler, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0,
                                   WICBitmapPaletteTypeCustom))) {
        std::vector<u8> px(size_t(kSize) * kSize * 4);
        if (SUCCEEDED(conv->CopyPixels(nullptr, kSize * 4, UINT(px.size()), px.data()))) {
            if (SDL_Surface* s = SDL_CreateSurfaceFrom(int(kSize), int(kSize), SDL_PIXELFORMAT_RGBA32, px.data(), int(kSize * 4))) {
                SDL_SetWindowIcon(window_, s); // (SDL copies the pixels)
                SDL_DestroySurface(s);
            }
        }
    }
    if (conv) conv->Release();
    if (scaler) scaler->Release();
    if (frame) frame->Release();
    if (dec) dec->Release();
    wic->Release();
}

bool Display::Init(const core::Config& cfg, const char* title) {
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | SDL_INIT_AUDIO)) {
        LOG_ERROR("Display", "SDL_Init failed: {}", SDL_GetError());
        return false;
    }
    SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    if (std::getenv("MUA3_HEADLESS")) // automated test runs: no visible window
        flags |= SDL_WINDOW_HIDDEN;
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
    // Waitable: the UI thread waits for a free frame slot before it draws, outside Present (which holds the D3D
    // device lock the GPU thread needs). Tearing (when the system allows it): vsync off shows frames at once.
    tearing_ = TearingSupported();
    sd.Flags = SwapChainFlags();
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
    live_cfg_ = &cfg;
    {
        IDXGISwapChain2* sc2 = nullptr;
        if (SUCCEEDED(swapchain_->QueryInterface(__uuidof(IDXGISwapChain2), (void**)&sc2))) {
            sc2->SetMaximumFrameLatency(1);
            frame_wait_ = sc2->GetFrameLatencyWaitableObject();
            sc2->Release();
        }
        LOG_INFO("Display", "vsync {}, tearing {}", cfg.vsync ? "on" : "off", tearing_ ? "supported" : "not supported");
    }
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

bool Display::TearingSupported() {
    IDXGIFactory5* f = nullptr;
    BOOL allow = FALSE;
    if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory5), (void**)&f))) {
        if (FAILED(f->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow, sizeof(allow))))
            allow = FALSE;
        f->Release();
    }
    return allow != FALSE;
}

u32 Display::SwapChainFlags() const {
    return DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH | DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT |
           (tearing_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
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
    SafeRelease(opaque_blend_);
    SafeRelease(rtv_);
    if (frame_wait_)
        CloseHandle(frame_wait_);
    frame_wait_ = nullptr;
    SafeRelease(swapchain_);
    SafeRelease(ctx_);
    SafeRelease(device_);
    if (window_)
        SDL_DestroyWindow(window_);
    window_ = nullptr;
    SDL_Quit();
}

u32 Display::PixelHeight() const {
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(window_, &w, &h);
    if (const SDL_DisplayMode* m = SDL_GetDesktopDisplayMode(SDL_GetDisplayForWindow(window_)); m && fullscreen_)
        h = std::max(h, int(m->h * m->pixel_density));
    return u32(std::max(h, 0));
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
            swapchain_->ResizeBuffers(0, 0, 0, DXGI_FORMAT_UNKNOWN, SwapChainFlags()); // (the same flags)
            CreateBackbuffer();
            break;
        case SDL_EVENT_KEY_DOWN:
            if ((e.key.key == SDLK_RETURN && (e.key.mod & SDL_KMOD_ALT)) || e.key.key == SDLK_F11)
                ToggleFullscreen();
            if (e.key.key == SDLK_F12 && !e.key.repeat) { // screenshot of the window as shown
                std::error_code ec;
                fs::create_directories("screenshots", ec);
                const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
                std::tm tm{};
                localtime_s(&tm, &now);
                char name[64];
                std::strftime(name, sizeof(name), "screenshots/mua3_%Y%m%d_%H%M%S.bmp", &tm);
                capture_path = name;
            }
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
    // Wait for the swap chain to take another frame (with vsync: the next vblank) before drawing, so Present
    // itself never blocks; the timeout keeps the UI going while the window is not presented (minimised).
    if (frame_wait_)
        WaitForSingleObjectEx(frame_wait_, 100, TRUE);
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
    // Opaque: the frame's alpha channel is whatever the game left there (its UI uses destination alpha as a mask,
    // e.g. the pause menu and the loading screen's picture panel leave it 0), and the Switch never shows it. ImGui's
    // alpha blending over the cleared backbuffer turned those areas black.
    if (!opaque_blend_) {
        D3D11_BLEND_DESC bd{};
        bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        device_->CreateBlendState(&bd, &opaque_blend_);
    }
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    dl->AddCallback(
        [](const ImDrawList*, const ImDrawCmd* cmd) {
            auto* self = static_cast<Display*>(cmd->UserCallbackData);
            self->ctx_->OMSetBlendState(self->opaque_blend_, nullptr, 0xFFFFFFFF);
        },
        this);
    dl->AddImage((ImTextureID)srv, p0, ImVec2(p0.x + w, p0.y + h));
    dl->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
}

// Writes the current backbuffer to a 24-bit BMP (automation: screenshots of hidden windows).
void Display::SaveBackbuffer(const fs::path& path) {
    ID3D11Texture2D* bb = nullptr;
    if (FAILED(swapchain_->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb)))
        return;
    D3D11_TEXTURE2D_DESC td;
    bb->GetDesc(&td);
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    td.MiscFlags = 0;
    ID3D11Texture2D* st = nullptr;
    if (SUCCEEDED(device_->CreateTexture2D(&td, nullptr, &st))) {
        ctx_->CopyResource(st, bb);
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(ctx_->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
            const u32 w = td.Width, h = td.Height, row = (w * 3 + 3) & ~3u;
            std::vector<u8> file(54 + size_t(row) * h, 0);
            auto put32 = [&](size_t o, u32 v) { std::memcpy(&file[o], &v, 4); };
            file[0] = 'B', file[1] = 'M';
            put32(2, u32(file.size())), put32(10, 54), put32(14, 40), put32(18, w), put32(22, h);
            file[26] = 1, file[28] = 24;
            const bool bgra = td.Format == DXGI_FORMAT_B8G8R8A8_UNORM || td.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
            for (u32 y = 0; y < h; y++) {
                const u8* src = static_cast<const u8*>(m.pData) + size_t(y) * m.RowPitch;
                u8* dst = &file[54 + size_t(h - 1 - y) * row];
                for (u32 x = 0; x < w; x++) {
                    dst[x * 3 + 0] = src[x * 4 + (bgra ? 0 : 2)];
                    dst[x * 3 + 1] = src[x * 4 + 1];
                    dst[x * 3 + 2] = src[x * 4 + (bgra ? 2 : 0)];
                }
            }
            ctx_->Unmap(st, 0);
            common::WriteFile(path, file);
        }
        st->Release();
    }
    bb->Release();
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
    if (!capture_path.empty()) {
        SaveBackbuffer(capture_path);
        capture_path.clear();
    }
    if (context_epoch)
        (*context_epoch)++;
    // Don't hold the context lock while Present waits for vblank: the GPU thread needs the context.
    if (lk.owns_lock())
        lk.unlock();
    // Never block inside Present: it holds the D3D device lock the GPU thread needs (BeginFrame waited for the
    // frame slot). VSync (mua3.ini / the in-game Graphics menu, read live): the next vblank; off: at once, with
    // tearing where the system allows it (else DWM composes at sync interval 0 without tearing).
    const bool vsync = live_cfg_ ? live_cfg_->vsync : true;
    swapchain_->Present(vsync ? 1 : 0, !vsync && tearing_ ? DXGI_PRESENT_ALLOW_TEARING : 0);
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
