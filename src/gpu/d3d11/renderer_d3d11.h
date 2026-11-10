#pragma once
#include <atomic>
#include <deque>
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
// Bumped by other users of the immediate context (the UI): invalidates the renderer's binding cache.
std::atomic<u64>& ContextEpoch();

// A render target or depth buffer living on the host GPU, keyed by its guest GPU address.
struct HostShader;

struct Surface {
    u64 gpu_va = 0, cpu_va = 0;
    u32 width = 0, height = 0, format = 0; // as the guest sees it
    u32 scale = 1;                         // host texture is width*scale x height*scale (resolution scaling)
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
    u64 sub_serial = 0; // write_serial the sub copy was taken at
    // Snapshot sampled while this surface is also bound as a target of the same draw (see HazardSrv).
    ComPtr<ID3D11Texture2D> copy_tex;
    ComPtr<ID3D11ShaderResourceView> copy_srv;
    u64 copy_serial = 0;  // write_serial the copy was taken at
    u64 write_serial = 1; // bumped by every draw or clear that may write the surface
    u64 last_use = 0;
};

class D3D11Renderer final : public Renderer {
public:
    D3D11Renderer(ID3D11Device* dev, ID3D11DeviceContext* ctx);
    ~D3D11Renderer() override;

    void ClearColor(u32 rt, const float color[4]) override;
    void ClearDepthStencil(bool depth, bool stencil, float depth_value, u8 stencil_value) override;
    void Draw() override;
    void Dispatch(const ComputeLaunch& l) override;
    // Internal resolution multiplier for screen-sized render targets (1 = native).
    void SetResolutionScale(u32 s) { res_scale_ = std::max<u32>(s, 1); }
    ShaderCache* Shaders() { return shaders_.get(); }
    void Present(const PresentInfo& p) override;
    void InvalidateRegion(u64 gpu_va, u64 size) override;
    bool Blit2D(const u32* regs) override;
    void FlushCommands() override;
    // At most kFramesInFlight game frames queued on the host GPU. Guest fences are host-side, so nothing else
    // stops the game from recording frames faster than the GPU runs them; the driver then keeps every renamed
    // WRITE_DISCARD buffer of every queued frame alive (8 MB chunks: up to ~20 GB of commit in gameplay).
    void ThrottleFrames() override;
    static constexpr u32 kFramesInFlight = 2;
    ComPtr<ID3D11Query> frame_queries_[kFramesInFlight + 1];
    u64 frame_query_count_ = 0;
    u64 throttle_wait_ns_ = 0, throttle_frames_ = 0;
    u32 CullListCount(u64 index_addr) override {
        auto it = cull_lists_.find(index_addr);
        return it == cull_lists_.end() ? 0 : it->second;
    }
    std::unordered_map<u64, u32> cull_lists_; // culling list start -> indices written there (EmulateCullingGs)
    struct CullSource {
        u64 addr;       // the culling draw's own indices (the list is those, as u32)
        u32 index_size; // bytes per source index
        u32 count;
    };
    std::unordered_map<u64, CullSource> cull_src_; // culling list start -> where its indices really are
    void ReportHitch();
    s64 shader_wait_ns_ = 0; // per frame: how long draws may still wait for shaders the game just asked for
    void SyncRegion(u64 gpu_va, u64 size) override {
        std::lock_guard ctx_lock(ContextMutex());
        SyncGuest(gpu_va, size, 11);
    }

    // Latest presented frame for the window (thread-safe).
    ID3D11ShaderResourceView* LatestFrame(u32& w, u32& h);
    std::mutex& FrameMutex() { return frame_mutex_; }
    // Signalled (auto-reset Win32 event) whenever Present makes a new frame available.
    void* NewFrameEvent() const { return new_frame_event_; }

    Surface* FindSurfaceByGpu(u64 gpu_va);
    // Resolution scale a 2D texture at this address is sampled with (render targets may be stored larger).
    u32 TextureScale(u64 gpu_va, bool tex2d);
    // A texture aliasing a render/depth target bound for the current draw: D3D11 would force that shader
    // resource to null (the guest legally samples e.g. depth while stencil-testing against it), so it reads a
    // copy taken just before the draw. Returns null when the surface is not bound.
    ID3D11ShaderResourceView* HazardSrv(Surface* s);
    Surface* FindSurfaceByCpu(u64 cpu_va);
    // Texel buffer over memory a compute kernel wrote: a typed view of the resident GPU copy (null if none).
    ID3D11ShaderResourceView* ComputeBufferSrv(u64 gpu_va, u64 size, DXGI_FORMAT fmt, u32 bpb);
    // Before reading guest memory on the host: lands pending compute results overlapping the range.
    void SyncGuest(u64 gpu_va, u64 size, int site = 0); // site: which consumer asked (profile)
    int sync_site_ = 0;
    u64 sync_va_ = 0; // (profile) address of the last SyncGuest

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
        recent_wanted_ = frame_counter_;
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
    common::U64Map<bool> gs_emits_; // geometry program address -> has OUT instructions
    common::U64Map<HostShader*> shader_by_addr_; // code address ^ stage -> shader
    u64 code_lo_ = ~0ull, code_hi_ = 0;                   // range of code those addresses cover
    void EmulateCullingGs();
    // Guest global memory used by compute (NVN storage buffers, image storage): raw UAV buffers that stay resident
    // on the host GPU, so chained kernels and texel-buffer reads use results without a round trip through guest
    // memory. Results are copied back asynchronously; CPU writes (dirty channel 1) re-upload the touched pages.
    struct GpuBuf {
        u64 addr = 0;
        u32 size = 0, capacity = 0;
        ComPtr<ID3D11Buffer> buf;
        ComPtr<ID3D11UnorderedAccessView> uav;
        std::vector<u8> snapshot; // guest contents as of the last sync, to write back only what kernels changed
        struct CpuRange {
            u32 off;
            u64 cpu, size;
        };
        std::vector<CpuRange> cpu_ranges;
        std::vector<std::pair<u32, u32>> cpu_pages; // (offset, size) runs the CPU wrote while results were in flight
        u32 in_flight = 0;          // queued readbacks not yet applied to guest memory
        bool gpu_dirty = false;     // kernels wrote it since the last copy-back was queued
        u32 serial = 0;             // bumped by every upload from guest memory
        std::vector<u32> page_serial; // per 4 KiB page: serial of its last upload
        bool stale = false;         // an overlapping buffer was written: re-upload everything
        bool retired = false;       // lies inside a larger buffer that now serves its windows
        bool gpu_written = false;
        // MUA3_RB_GPUDIFF: GPU copy of what guest memory holds (as of the last copy-back plus re-uploads), so a
        // kernel pass can flag the 4 KiB pages that changed and copy-backs compare only those on the CPU.
        ComPtr<ID3D11Buffer> prev;
        ComPtr<ID3D11ShaderResourceView> buf_srv, prev_srv; // raw views
        ID3D11Buffer* buf_srv_of = nullptr; // the buffer buf_srv views
        u32 prev_capacity = 0;
        bool diff_full = true;          // the next copy-back compares every page (prev is not in step)
        std::vector<u32> mask_acc;      // flags of superseded copy-backs, still to be compared
        bool apply_full = false;        // a superseded copy-back's flags were not available: compare everything
        common::U64Map<ComPtr<ID3D11ShaderResourceView>> srvs; // typed views for texel buffers
        u64 last_use = 0;
    };
    std::vector<std::unique_ptr<GpuBuf>> gbufs_;
    std::vector<GpuBuf*> active_; // (SyncGuest) buffers with results in flight or not copied back yet
    bool active_stale_ = true;    // set by anything that changes which buffers those are, or frees one
    bool in_sync_ = false;
    struct Readback {
        GpuBuf* b;
        ComPtr<ID3D11Buffer> staging;
        u32 size;
        u32 serial; // buffer's upload serial when the copy was taken
        ComPtr<ID3D11Buffer> mask; // staging: u32 per page, nonzero = changed (MUA3_RB_GPUDIFF), null = compare all
        u32 pages = 0;
    };
    std::vector<ComPtr<ID3D11Buffer>> mask_pool_;
    ComPtr<ID3D11ComputeShader> diff_cs_;
    ComPtr<ID3D11Buffer> diff_mask_, diff_cb_;
    ComPtr<ID3D11UnorderedAccessView> diff_mask_uav_;
    u32 diff_mask_pages_ = 0;
    // Flags the pages of `b` that differ from b.prev into a staging buffer, then brings prev up to date. Null if off.
    ComPtr<ID3D11Buffer> DiffPages(GpuBuf& b, u32& pages);
    std::deque<Readback> readbacks_;
    std::vector<ComPtr<ID3D11Buffer>> staging_pool_;
    GpuBuf* ComputeBuffer(u64 addr, u32 size);
    // The resident compute buffer holding [addr, addr + size), up to date with CPU writes, for drawing from directly.
    GpuBuf* ResidentBuffer(u64 addr, u64 size);
    // Non-indexed quads with indirect arguments (particles): a one-thread compute pass turns the arguments into
    // those of an indexed draw over a fixed quad->triangle index list, so nothing waits on the GPU.
    bool PrepareQuadIndirect(GpuBuf& args, u32 off);
    ComPtr<ID3D11ComputeShader> quad_args_cs_;
    ComPtr<ID3D11Buffer> quad_args_buf_, quad_index_buf_;
    ComPtr<ID3D11UnorderedAccessView> quad_args_uav_;
    void Reupload(GpuBuf& b, bool all, u32 from = 0); // all && from: only the new tail [from, size)
    // Applies finished readbacks in order; `wait` blocks for all of them, `until` for those of one buffer.
    void PollReadbacks(bool wait, const GpuBuf* until = nullptr);
    void ApplyReadback(GpuBuf& b, const u8* now, u32 size, u32 serial, const u32* mask = nullptr, u32 pages = 0);
    void QueueReadback(GpuBuf& b);
    void FlushGpuBuf(GpuBuf& b); // results of `b` into guest memory now (blocking)
    bool gpu_dirty_any_ = false;
    u32 draws_since_poll_ = 0;
    // Shader-stage bindings last set on the context (VS, PS), to skip redundant Set* calls.
    struct StageBinds {
        ID3D11Buffer* cb[14];
        ID3D11ShaderResourceView* srv[48];
        ID3D11SamplerState* smp[16];
    };
    StageBinds binds_[2]{};
    u64 binds_epoch_ = ~0ull;
    // What the context's fixed-function and shader stages hold (consecutive draws mostly share it): state is
    // set only when it differs. Filled with 0xFF (matches nothing) whenever the context may have changed.
    struct PipeState {
        ID3D11BlendState* blend;
        float blend_color[4];
        ID3D11DepthStencilState* dss;
        u32 stencil_ref;
        ID3D11RasterizerState* rs;
        ID3D11InputLayout* layout;
        ID3D11VertexShader* vs;
        ID3D11PixelShader* ps;
        ID3D11GeometryShader* gs;
        D3D11_VIEWPORT vp;
        D3D11_RECT scissor;
        ID3D11Buffer* vb[32];
        UINT vb_stride[32], vb_offset[32];
        bool valid;
    };
    PipeState pipe_{};
    void InvalidatePipe() {
        std::memset(&pipe_, 0xFF, sizeof(pipe_));
        pipe_.valid = true;
    }
    ID3D11RenderTargetView* last_rtvs_[8]{};
    ID3D11DepthStencilView* last_dsv_ = nullptr;
    void InvalidateBinds() { binds_epoch_ = ~0ull; }
    // Guest index and vertex buffers kept on the host GPU between draws. Pages the CPU rewrites (dirty channel 2)
    // are re-uploaded; a buffer that is rewritten nearly every use is "streaming" and goes back to per-draw uploads.
    struct GeomBuf {
        u64 addr = 0;
        u32 size = 0;
        ComPtr<ID3D11Buffer> buf;
        std::vector<GpuBuf::CpuRange> cpu_ranges;
        u32 dirty_streak = 0;
        bool streaming = false;
        u64 last_use = 0, first_seen = ~0ull;
        DXGI_FORMAT index_format = DXGI_FORMAT_UNKNOWN; // index buffers: format and vertex range
        u32 min_v = 0, max_v = 0, index_size = 0;
    };
    common::U64Map<GeomBuf> index_cache_, vertex_cache_;
    // Last upload per (stage, constant buffer slot), to skip re-uploading identical contents.
    struct CbShadow {
        u64 addr = 0;
        std::vector<u8> data;
        ID3D11Buffer* buf = nullptr;
        ID3D11ShaderResourceView* raw = nullptr;
    };
    CbShadow cb_shadow_[160];
    GeomBuf* CachedIndices(u64 addr, u32 count, u32 index_size, bool strip);
    GeomBuf* CachedVertices(u64 addr, u32 size);
    // Creates/refreshes `b` from guest memory: true when usable (not streaming). `fill` re-reads all of it.
    bool RefreshGeometry(GeomBuf& b, D3D11_BIND_FLAG bind, bool& fill);
    common::U64Map<HostShader*> compute_by_key_; // program address ^ launch shape -> shader
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
    common::U64Map<std::unique_ptr<Surface>> surfaces_;
    u64 frame_counter_ = 0;
    Surface* bound_[9]{}; // colour targets 0-7 and depth of the draw being set up
    u64 draw_serial_ = 0;
    u64 surface_writes_ = 1;
    u32 res_scale_ = 1;
    void* new_frame_event_ = nullptr;

    std::unique_ptr<ShaderCache> shaders_;
    std::unique_ptr<TextureCache> textures_;

    ComPtr<ID3D11PixelShader> debug_ps_;
    int debug_mode_ = 0;
    bool trace_draw_ = false;
    u64 last_present_cpu_ = 0;
    std::set<u64> presented_cpu_;
    std::mutex frame_mutex_;
    std::mutex recent_mutex_;
    std::atomic<u64> recent_wanted_{~0ull >> 1}; // frame the overlay last asked for RecentTextures()
    std::vector<RecentTexture> recent_;
    ComPtr<ID3D11Texture2D> frame_tex_;
    ComPtr<ID3D11ShaderResourceView> frame_srv_;
    u32 frame_w_ = 0, frame_h_ = 0;

    friend class DrawContext;
};

} // namespace gpu
