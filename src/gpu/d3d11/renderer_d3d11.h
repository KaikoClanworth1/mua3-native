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
struct HostShader;

struct Surface {
    u64 gpu_va = 0, cpu_va = 0;
    u32 width = 0, height = 0, format = 0;
    bool depth = false;
    DXGI_FORMAT dxgi = DXGI_FORMAT_UNKNOWN;
    ComPtr<ID3D11Texture2D> tex;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11DepthStencilView> dsv;
    ComPtr<ID3D11ShaderResourceView> srv;
    // Copy of the top-left region for textures that alias this target with a smaller size (the game may
    // render a 1x1 value into a 16x1 target): sampling the full target would read unwritten texels.
    ComPtr<ID3D11Texture2D> sub_tex;
    ComPtr<ID3D11ShaderResourceView> sub_srv;
    u32 sub_w = 0, sub_h = 0;
    // Snapshot sampled while this surface is also bound as a target of the same draw (see HazardSrv).
    ComPtr<ID3D11Texture2D> copy_tex;
    ComPtr<ID3D11ShaderResourceView> copy_srv;
    u64 copy_serial = ~0ull;
    u64 last_use = 0;
};

class D3D11Renderer final : public Renderer {
public:
    D3D11Renderer(ID3D11Device* dev, ID3D11DeviceContext* ctx);
    ~D3D11Renderer() override;

    void Clear(u32 rt, u32 mask) override;
    void Draw() override;
    void Dispatch(const u32* regs, const u32* qmd) override;
    void Present(const PresentInfo& p) override;
    void InvalidateRegion(u64 gpu_va, u64 size) override;
    bool Blit2D(const u32* regs) override;
    void FlushCommands() override;

    // Latest presented frame for the window (thread-safe).
    ID3D11ShaderResourceView* LatestFrame(u32& w, u32& h);
    std::mutex& FrameMutex() { return frame_mutex_; }

    Surface* FindSurfaceByGpu(u64 gpu_va);
    // A texture aliasing a render/depth target bound for the current draw: D3D11 would force that shader
    // resource to null (the guest legally samples e.g. depth while stencil-testing against it), so it reads a
    // copy taken just before the draw. Returns null when the surface is not bound.
    ID3D11ShaderResourceView* HazardSrv(Surface* s);
    Surface* FindSurfaceByCpu(u64 cpu_va);

    void DumpSurface(Surface* s, const fs::path& path);
    // Saves the next presented frame (debug / automation).
    void RequestFrameShot(const fs::path& path) {
        std::lock_guard lk(shot_mutex_);
        shot_path_ = path;
    }
    std::mutex shot_mutex_;
    fs::path shot_path_;
    std::atomic<bool> dump_surfaces_{false};
    // Debug: log the raw texel at (x, y) of surface `gpu_va` at the next present (mua3_cmd "pixel").
    void ReadPixel(u64 gpu_va, u32 x, u32 y) { std::lock_guard l(pixel_mutex_); pixel_reqs_.push_back({gpu_va, x, y}); }
    std::atomic<int> trace_draws_{0}; // debug: log the next N draws into the presented surface
    std::atomic<u64> trace_rt_{0};    // ... or into this surface (gpu va) when set
    // Debug pixel history: draws of frame `pick_frame_` touching (pick_x_, pick_y_) (fractions of the target size).
    void Pick(float x, float y) { pick_x_ = x; pick_y_ = y; pick_frame_ = frame_counter_ + 2; }
    void CountSamples() { occl_frame_ = frame_counter_ + 2; }

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
    std::atomic<u64> pick_frame_{~0ull}, occl_frame_{~0ull};
    std::string trace_vb_;
    std::vector<u8> trace_vb_data_;
    u32 trace_vb_stride_ = 0;
    u64 skip_reason_[6]{}; // shader not ready, topology, no indices, index range, input layout, GS without output
    std::unordered_map<u64, bool> gs_emits_; // geometry program address -> has OUT instructions
    std::unordered_map<u64, HostShader*> shader_by_addr_; // code address ^ stage -> shader
    u64 code_lo_ = ~0ull, code_hi_ = 0;                   // range of code those addresses cover
    void EmulateCullingGs();
    // Guest global memory windows for compute (NVN storage buffers): raw UAV buffers mirrored from guest memory.
    struct GmemWindow {
        ComPtr<ID3D11Buffer> buf, staging;
        ComPtr<ID3D11UnorderedAccessView> uav;
        u32 capacity = 0;
        std::vector<u8> uploaded; // guest bytes as uploaded, to write back only what the dispatch changed
    };
    GmemWindow gmem_[8]; // shader::kGmemWindows
    std::unordered_map<u64, HostShader*> compute_by_key_; // program address ^ launch shape -> shader
    u64 dispatches_ = 0;
    float pick_x_ = 0, pick_y_ = 0;
    struct PixelReq {
        u64 va;
        u32 x, y;
    };
    std::mutex pixel_mutex_;
    std::vector<PixelReq> pixel_reqs_;
    Surface* GetColorSurface(u32 index);
    Surface* GetDepthSurface();
    Surface* GetSurface(u64 gpu_va, u32 width, u32 height, u32 format, bool depth, u32 tile_mode = 0);
    void BindRenderTargets(Surface** colors, u32 count, Surface* depth);

    ID3D11Device* dev_;
    ID3D11DeviceContext* ctx_;
    std::unordered_map<u64, std::unique_ptr<Surface>> surfaces_;
    u64 frame_counter_ = 0;
    Surface* bound_[9]{}; // colour targets 0-7 and depth of the draw being set up
    u64 draw_serial_ = 0;

    std::unique_ptr<ShaderCache> shaders_;
    std::unique_ptr<TextureCache> textures_;

    ComPtr<ID3D11PixelShader> debug_ps_;
    int debug_mode_ = 0;
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
