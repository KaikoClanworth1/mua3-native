#pragma once
#include <atomic>
#include <set>
#include <memory>
#include <mutex>
#include <unordered_map>

#include <d3d11.h>
#include <wrl/client.h>

#include "gpu/renderer.h"

namespace gpu {

using Microsoft::WRL::ComPtr;

class ShaderCache;
class TextureCache;

// Serializes use of the D3D11 immediate context between the GPU thread and the UI thread.
std::recursive_mutex& ContextMutex();

// A render target or depth buffer living on the host GPU, keyed by its guest GPU address.
struct Surface {
    u64 gpu_va = 0, cpu_va = 0;
    u32 width = 0, height = 0, format = 0;
    bool depth = false;
    DXGI_FORMAT dxgi = DXGI_FORMAT_UNKNOWN;
    ComPtr<ID3D11Texture2D> tex;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11DepthStencilView> dsv;
    ComPtr<ID3D11ShaderResourceView> srv;
    u64 last_use = 0;
};

class D3D11Renderer final : public Renderer {
public:
    D3D11Renderer(ID3D11Device* dev, ID3D11DeviceContext* ctx);
    ~D3D11Renderer() override;

    void Clear(u32 rt, u32 mask) override;
    void Draw() override;
    void Present(const PresentInfo& p) override;
    void InvalidateRegion(u64 gpu_va, u64 size) override;
    bool Blit2D(const u32* regs) override;
    void FlushCommands() override;

    // Latest presented frame for the window (thread-safe).
    ID3D11ShaderResourceView* LatestFrame(u32& w, u32& h);
    std::mutex& FrameMutex() { return frame_mutex_; }

    Surface* FindSurfaceByGpu(u64 gpu_va);
    Surface* FindSurfaceByCpu(u64 cpu_va);

    void DumpSurface(Surface* s, const fs::path& path);
    std::atomic<bool> dump_surfaces_{false};

    // Debug: recently sampled textures / render targets (for the overlay's texture viewer).
    struct RecentTexture {
        ComPtr<ID3D11ShaderResourceView> srv;
        u64 addr;
        u32 w, h, fmt;
        bool rt;
    };
    std::vector<RecentTexture> RecentTextures() {
        std::lock_guard lk(recent_mutex_);
        return recent_;
    }
    void NoteTexture(ID3D11ShaderResourceView* srv, u64 addr, u32 w, u32 h, u32 fmt, bool rt);

    // Stats
    u64 draws_ok = 0, draws_skipped = 0, clears = 0;

private:
    Surface* GetColorSurface(u32 index);
    Surface* GetDepthSurface();
    Surface* GetSurface(u64 gpu_va, u32 width, u32 height, u32 format, bool depth);
    void BindRenderTargets(Surface** colors, u32 count, Surface* depth);

    ID3D11Device* dev_;
    ID3D11DeviceContext* ctx_;
    std::unordered_map<u64, std::unique_ptr<Surface>> surfaces_;
    u64 frame_counter_ = 0;

    std::unique_ptr<ShaderCache> shaders_;
    std::unique_ptr<TextureCache> textures_;

    ComPtr<ID3D11PixelShader> debug_ps_;
    int debug_mode_ = 0;
    int trace_draws_ = 0;
    bool trace_draw_ = false;
    u64 last_present_cpu_ = 0;
    std::set<u64> presented_cpu_;
    std::mutex frame_mutex_;
    std::mutex recent_mutex_;
    std::vector<RecentTexture> recent_;
    ComPtr<ID3D11Texture2D> frame_tex_;
    ComPtr<ID3D11ShaderResourceView> frame_srv_;
    u32 frame_w_ = 0, frame_h_ = 0;

    friend class DrawContext;
};

} // namespace gpu
