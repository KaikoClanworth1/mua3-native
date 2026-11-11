#pragma once
#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include <d3d11.h>
#include <wrl/client.h>

#include "gpu/shader/translator.h"

namespace gpu {

using Microsoft::WRL::ComPtr;
class D3D11Renderer;

struct TicEntry {
    std::array<u32, 8> w{};
    u32 Format() const { return w[0] & 0x7F; }
    u32 RType() const { return (w[0] >> 7) & 7; }
    u32 Swizzle() const { return (w[0] >> 19) & 0xFFF; } // x,y,z,w sources, 3 bits each
    u64 Address() const { return (u64(w[2] & 0xFFFF) << 32) | w[1]; }
    u32 HeaderVersion() const { return (w[2] >> 21) & 7; }
    u32 BlockHeight() const { return (w[3] >> 3) & 7; }
    u32 BlockDepth() const { return (w[3] >> 6) & 7; }
    u32 MaxMip() const { return (w[3] >> 28) & 0xF; }
    u32 Pitch() const { return (w[3] & 0xFFFF) << 5; }
    // Header version 0 is a 1D buffer: linear, no pitch, and word 3 holds the upper half of the width.
    bool IsBuffer() const { return HeaderVersion() == 0; }
    u32 Width() const { return (IsBuffer() ? ((w[3] & 0xFFFF) << 16) | (w[4] & 0xFFFF) : (w[4] & 0xFFFF)) + 1; }
    bool Srgb() const { return (w[4] >> 22) & 1; }
    u32 Type() const { return (w[4] >> 23) & 0xF; }
    u32 Height() const { return (w[5] & 0xFFFF) + 1; }
    u32 Depth() const { return ((w[5] >> 16) & 0x3FFF) + 1; }
};

struct CachedTexture {
    u64 key = 0;
    u64 gpu_va = 0, size = 0;
    std::vector<std::pair<u64, u64>> cpu_ranges; // guest CPU ranges backing it (write-tracked)
    ComPtr<ID3D11Resource> res;
    ComPtr<ID3D11ShaderResourceView> srv;
    u64 last_use = 0;
    bool updatable = false; // DEFAULT usage: rewrites are decoded into it in place
    std::vector<u64> page_hash; // per backing 4 KiB page, as decoded (rewrites that change nothing are skipped)
};

class TextureCache {
public:
    TextureCache(ID3D11Device* dev, ID3D11DeviceContext* ctx, D3D11Renderer& r);
    ~TextureCache();
    // Returns an SRV for the TIC entry (or null). `swizzle_out` receives the packed component swizzle.
    ID3D11ShaderResourceView* Get(const TicEntry& tic, const shader::TextureUse& use, u32& swizzle_out);
    ID3D11SamplerState* GetSampler(const u32* tsc, bool compare);
    void Invalidate(u64 gpu_va, u64 size);
    void NextFrame();
    u64 reuploads = 0, uploads = 0, unchanged_rewrites = 0, tracked_bytes = 0;

private:
    // `reuse`: decode into this existing (updatable) texture; `updatable`: create with DEFAULT usage.
    // `out`: build the texture there instead of adding it to the cache (worker threads).
    ID3D11ShaderResourceView* Upload(const TicEntry& tic, const shader::TextureUse& use, u64 key, CachedTexture* reuse = nullptr,
                                     bool updatable = false, std::unique_ptr<CachedTexture>* out = nullptr);
    // Asynchronous uploads (MUA3_TEX_BUDGET_MS per frame of synchronous upload time, default 8, 0 = all synchronous):
    // a load that brings hundreds of textures at once spreads them over a few frames instead of one long one.
    struct AsyncJob {
        TicEntry tic;
        shader::TextureUse use;
        u64 key, gen;
    };
    struct AsyncDone {
        u64 key, gen;
        std::unique_ptr<CachedTexture> tex;
    };
    void AsyncWorker();
    void InstallAsync();
    ID3D11ShaderResourceView* Placeholder(const shader::TextureUse& use);
    u64 budget_ns_ = 0;
    std::mutex aq_mutex_;
    std::condition_variable aq_cv_;
    std::deque<AsyncJob> aq_;
    std::vector<AsyncDone> done_;
    std::atomic<bool> has_done_{false};
    bool stop_ = false;
    std::vector<std::thread> workers_;
    std::unordered_set<u64> pending_;                    // keys queued or being built
    u64 inval_gen_ = 0;                                  // Invalidate() calls so far
    std::vector<std::pair<u64, std::pair<u64, u64>>> inval_log_; // (generation, (gpu va, size)) while jobs are pending
    ComPtr<ID3D11ShaderResourceView> placeholder_;
    u64 async_queued_ = 0, async_installed_ = 0, async_dropped_ = 0;
    ID3D11Device* dev_;
    ID3D11DeviceContext* ctx_;
    D3D11Renderer& r_;
    common::U64Map<std::unique_ptr<CachedTexture>> textures_;
    common::U64Map<int> pages_; // 64 KB GPU pages covered by cached textures -> texture count
    void TrackPages(const CachedTexture& t, int delta);
    void Erase(common::U64Map<std::unique_ptr<CachedTexture>>::iterator& it);
    common::U64Map<ComPtr<ID3D11SamplerState>> samplers_;
    ID3D11ShaderResourceView* Fallback(const shader::TextureUse& use);
    ComPtr<ID3D11ShaderResourceView> fallback_;
    ComPtr<ID3D11ShaderResourceView> fallbacks_[5]; // 2D, 3D, cube, 2D array, buffer
    u64 frame_ = 0;
    // this frame's uploads (ns): guest read + deswizzle/decode, driver create, write tracking, page hashes; count, bytes
    u64 up_ns_[4]{}, up_count_ = 0, up_bytes_ = 0;
};

} // namespace gpu
