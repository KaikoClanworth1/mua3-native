#include "gpu/d3d11/texture_cache.h"

#include <chrono>
#include <windows.h>
#include <map>
#include <set>

#include "core/memory.h"
#include "gpu/d3d11/renderer_d3d11.h"
#include "gpu/d3d11/shader_cache.h"
#include "gpu/d3d11/texture_stats.h"
#include "gpu/formats.h"
#include "gpu/gpu.h"

namespace gpu {

TextureCache::TextureCache(ID3D11Device* dev, ID3D11DeviceContext* ctx, D3D11Renderer& r)
    : dev_(dev), ctx_(ctx), r_(r) {
    const char* b = std::getenv("MUA3_TEX_BUDGET_MS");
    budget_ns_ = u64(b ? std::atof(b) : 8.0) * 1'000'000;
    { // (#8 diagnostics: which way CPU-rewritten textures are uploaded again; Get reads the same variables)
        const bool full = std::getenv("MUA3_TEX_FULL") != nullptr, staging = std::getenv("MUA3_TEX_STAGING") != nullptr;
        static bool logged = false;
        if (!logged) {
            logged = true;
            LOG_INFO("Texture", "upload path {} ({})",
                     staging ? (full ? "staging, whole textures" : "staging") : full ? "full" : "default (partial)",
                     staging || full ? "from env" : "no MUA3_TEX_FULL / MUA3_TEX_STAGING");
        }
    }
    if (budget_ns_)
        for (int i = 0; i < 2; i++)
            workers_.emplace_back([this] { AsyncWorker(); });
    // 1x1 opaque grey used when a texture can't be decoded.
    D3D11_TEXTURE2D_DESC td{1, 1, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D11_USAGE_IMMUTABLE,
                            D3D11_BIND_SHADER_RESOURCE, 0, 0};
    const u32 px = 0xFF808080;
    D3D11_SUBRESOURCE_DATA sd{&px, 4, 4};
    ComPtr<ID3D11Texture2D> t;
    dev_->CreateTexture2D(&td, &sd, &t);
    dev_->CreateShaderResourceView(t.Get(), nullptr, &fallback_);
}

void TextureCache::TrackPages(const CachedTexture& t, int delta) {
    if (!t.size)
        return;
    for (u64 p = t.gpu_va >> 16, e = (t.gpu_va + t.size - 1) >> 16; p <= e; p++) {
        auto& n = pages_[p];
        n += delta;
        if (n <= 0)
            pages_.erase(p);
    }
}

void TextureCache::Erase(common::U64Map<std::unique_ptr<CachedTexture>>::iterator& it) {
    TrackPages(*it->second, -1);
    it = textures_.erase(it);
}

TextureCache::~TextureCache() {
    {
        std::lock_guard lk(aq_mutex_);
        stop_ = true;
    }
    aq_cv_.notify_all();
    for (auto& t : workers_)
        t.join();
}

void TextureCache::Invalidate(u64 gpu_va, u64 size) {
    if (!size)
        return;
    {
        std::lock_guard lk(aq_mutex_);
        inval_gen_++;
        if (!pending_.empty())
            inval_log_.push_back({inval_gen_, {gpu_va, size}});
    }
    // Most writes (constant uploads, index lists) touch no cached texture: check the page index first.
    bool hit = false;
    for (u64 p = gpu_va >> 16, e = (gpu_va + size - 1) >> 16; p <= e && !hit; p++)
        hit = pages_.count(p) != 0;
    if (!hit)
        return;
    for (auto it = textures_.begin(); it != textures_.end();) {
        auto& t = *it->second;
        if (!(t.gpu_va < gpu_va + size && gpu_va < t.gpu_va + t.size)) {
            ++it;
            continue;
        }
        // (#8) a texture rewritten this way before is updatable: keep it and decode into it again on its next use,
        // instead of a new D3D11 texture per rewrite (VIC writes each video frame; the AMD driver keeps the old ones)
        if (t.updatable && !no_reuse_) {
            if (const u64 movie = MovieWatch()) { // (#22) per movie, its first 3 rewritten video textures (luma, chroma)
                static u64 movie_seen = 0;
                static std::vector<u64> seen;
                if (movie_seen != movie)
                    movie_seen = movie, seen.clear();
                if (seen.size() < 3 && std::find(seen.begin(), seen.end(), t.gpu_va) == seen.end()) {
                    seen.push_back(t.gpu_va);
                    LOG_INFO("Texture", "movie texture rewritten: texture {:#x}+{:#x} (cpu {:#x}) by a write to {:#x}+{:#x} "
                                        "(cpu {:#x}, movie plane {})", t.gpu_va, t.size, G().As().Translate(t.gpu_va), gpu_va,
                             size, G().As().Translate(gpu_va), MoviePlaneAt(G().As().Translate(gpu_va)));
                }
            }
            t.dma_dirty = true;
            ++it;
            continue;
        }
        if (invalidated_keys_.size() >= 4096)
            invalidated_keys_.clear();
        invalidated_keys_.insert(t.key);
        Erase(it);
    }
}

static u64 NowNs() { return u64(std::chrono::steady_clock::now().time_since_epoch().count()); }

void TextureCache::NextFrame() {
    const u64 total = up_ns_[0] + up_ns_[1] + up_ns_[2] + up_ns_[3];
    static u64 logged_queued = 0;
    if (total > 20'000'000 || (async_queued_ != logged_queued && frame_ % 300 == 0)) {
        logged_queued = async_queued_;
        LOG_INFO("Texture", "{} uploads ({:.1f} MB) took {:.1f} ms (tracking + hashes + read + decode {:.1f}, create {:.1f}); "
                            "asynchronous so far: {} queued, {} installed, {} dropped",
                 up_count_, up_bytes_ / 1048576.0, total / 1e6, up_ns_[0] / 1e6, up_ns_[1] / 1e6, async_queued_,
                 async_installed_, async_dropped_);
    }
    static u64 logged_dma = 0;
    if (dma_reuploads_ != logged_dma && frame_ % 600 == 0) {
        logged_dma = dma_reuploads_;
        LOG_INFO("Texture", "rewritten by the GPU or DMA (video frames): {} decoded into the same texture so far", dma_reuploads_);
    }
    std::fill(std::begin(up_ns_), std::end(up_ns_), 0);
    up_count_ = up_bytes_ = 0;
    frame_++;
}

ID3D11ShaderResourceView* TextureCache::Fallback(const shader::TextureUse& use) {
    const int kind = use.dim == shader::TexDim::Buffer ? 4 : use.dim == shader::TexDim::Tex3D ? 1
                   : use.dim == shader::TexDim::Cube ? 2 : use.array ? 3 : 0;
    if (fallbacks_[kind])
        return fallbacks_[kind].Get();
    const u32 px[6] = {0xFF808080, 0xFF808080, 0xFF808080, 0xFF808080, 0xFF808080, 0xFF808080};
    D3D11_SUBRESOURCE_DATA sd[6];
    for (auto& d : sd)
        d = {px, 4, 4};
    if (kind == 4) {
        const u32 zero[4]{};
        D3D11_BUFFER_DESC bd{16, D3D11_USAGE_IMMUTABLE, D3D11_BIND_SHADER_RESOURCE, 0, 0, 0};
        const D3D11_SUBRESOURCE_DATA bdata{zero, 0, 0};
        ComPtr<ID3D11Buffer> b;
        dev_->CreateBuffer(&bd, &bdata, &b);
        D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.Format = DXGI_FORMAT_R32_UINT;
        sv.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        sv.Buffer.NumElements = 4;
        dev_->CreateShaderResourceView(b.Get(), &sv, &fallbacks_[kind]);
    } else if (kind == 1) {
        D3D11_TEXTURE3D_DESC td{1, 1, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_USAGE_IMMUTABLE, D3D11_BIND_SHADER_RESOURCE, 0, 0};
        ComPtr<ID3D11Texture3D> t;
        dev_->CreateTexture3D(&td, sd, &t);
        dev_->CreateShaderResourceView(t.Get(), nullptr, &fallbacks_[kind]);
    } else {
        const UINT layers = kind == 2 ? 6 : 1;
        D3D11_TEXTURE2D_DESC td{1, 1, 1, layers, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D11_USAGE_IMMUTABLE,
                                D3D11_BIND_SHADER_RESOURCE, 0, kind == 2 ? D3D11_RESOURCE_MISC_TEXTURECUBE : 0u};
        ComPtr<ID3D11Texture2D> t;
        dev_->CreateTexture2D(&td, sd, &t);
        D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        if (kind == 2) sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE, sv.TextureCube = {0, 1};
        else if (kind == 3) sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY, sv.Texture2DArray = {0, 1, 0, 1};
        else sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D, sv.Texture2D = {0, 1};
        dev_->CreateShaderResourceView(t.Get(), &sv, &fallbacks_[kind]);
    }
    return fallbacks_[kind].Get();
}

// Hash of a texture's guest bytes, to tell a real rewrite from writes that left it as it was (neighbouring data on
// the same pages, or the same contents written again).
static u64 ContentHash(const u8* p, size_t n) {
    u64 h[4] = {0x9E3779B97F4A7C15ull, 0xC2B2AE3D27D4EB4Full, 0x165667B19E3779F9ull, 0x27D4EB2F165667C5ull};
    size_t i = 0;
    for (; i + 32 <= n; i += 32)
        for (int k = 0; k < 4; k++) {
            u64 w;
            std::memcpy(&w, p + i + k * 8, 8);
            h[k] = (h[k] ^ w) * 0x100000001B3ull;
            h[k] ^= h[k] >> 31;
        }
    u64 r = h[0] ^ (h[1] * 3) ^ (h[2] * 5) ^ (h[3] * 7) ^ n;
    for (; i < n; i++)
        r = (r ^ p[i]) * 0x100000001B3ull;
    return r ^ (r >> 29);
}

// Per 4 KiB page of the texture's backing CPU memory (cpu_ranges order): a write marks pages dirty, and only those
// are hashed again to tell whether the texture changed (a whole 4 MB texture was hashed for one written page).
template <typename F>
static void ForEachPage(const CachedTexture& t, F f) {
    size_t k = 0;
    for (auto& [a, n] : t.cpu_ranges)
        for (u64 off = 0; off < n; k++) {
            const u64 chunk = std::min<u64>(n - off, 0x1000 - ((a + off) & 0xFFF));
            f(k, a + off, chunk);
            off += chunk;
        }
}
static void HashPages(CachedTexture& t) {
    t.page_hash.clear();
    ForEachPage(t, [&](size_t, u64 cpu, u64 n) { t.page_hash.push_back(ContentHash(G().Mem().Ptr(cpu), size_t(n))); });
}
// The dirty pages whose contents differ from when the texture was decoded, as (offset, size) in the texture: each dirty
// page is write-tracked again first, then re-hashed (its new hash kept), so a write that lands meanwhile marks it
// dirty again instead of being lost. Returns the bytes tracked again.
static u64 ChangedPages(CachedTexture& t, std::vector<std::pair<u64, u64>>& out) {
    out.clear();
    u64 off = 0, tracked = 0;
    ForEachPage(t, [&](size_t k, u64 cpu, u64 n) {
        const u64 o = off;
        off += n;
        if (!G().Mem().ConsumeDirty(cpu, n))
            return;
        G().Mem().TrackWrites(cpu, n);
        tracked += n;
        const u64 h = ContentHash(G().Mem().Ptr(cpu), size_t(n));
        if (k < t.page_hash.size() && h == t.page_hash[k])
            return;
        if (k < t.page_hash.size())
            t.page_hash[k] = h;
        if (!out.empty() && out.back().first + out.back().second == o)
            out.back().second += n;
        else
            out.emplace_back(o, n);
    });
    return tracked;
}

ID3D11ShaderResourceView* TextureCache::Get(const TicEntry& tic, const shader::TextureUse& use, u32& swizzle) {
    if (has_done_.load(std::memory_order_acquire))
        InstallAsync();
    // Pack x/y/z/w component sources (3 bits each) for the shader's ApplySwizzle.
    swizzle = tic.Swizzle();
    const u64 addr = tic.Address();
    if (use.dim == shader::TexDim::Buffer) {
        // Texel buffer over compute output: read the resident GPU copy.
        const TexFormatInfo fi = TicFormat(tic.Format(), tic.RType(), tic.Srgb());
        if (auto* v = r_.ComputeBufferSrv(addr, u64(tic.Width()) * fi.bytes_per_block, fi.dxgi, fi.bytes_per_block))
            return v;
    }
    r_.SyncGuest(addr, 1, 1);
    // (#22) once per movie and plane: how the game's shader gets it (only while a movie's planes are still unreported)
    if (const u64 movie = MovieWatch()) {
        static u64 done[3] = {};
        const int plane = MoviePlaneAt(G().As().Translate(addr));
        if (plane && done[plane] != movie) {
            done[plane] = movie;
            const Surface* s = r_.FindSurfaceByGpu(addr);
            const bool by_surface = s && s->srv && use.dim == shader::TexDim::Tex2D && !use.array;
            LOG_INFO("Texture", "movie {} plane {:#x}: texture {}x{} format {:#x} swizzle {:#x}, {}", plane == 1 ? "luma" : "chroma",
                     addr, tic.Width(), tic.Height(), tic.Format(), tic.Swizzle(),
                     by_surface ? fmt::format("served by a render target {}x{} format {:#x}", s->width, s->height, s->format)
                                : std::string("from the texture cache"));
        }
    }
    if (use.dim == shader::TexDim::Tex3D) {
        static int n = 0;
        if (n++ < 4)
            LOG_INFO("Texture", "3D use: tic addr {:#x} type {} fmt {:#x} {}x{}x{} surface {}", addr, tic.Type(), tic.Format(),
                     tic.Width(), tic.Height(), tic.Depth(), (void*)r_.FindSurfaceByGpu(addr));
    }
    if (Surface* s = r_.FindSurfaceByGpu(addr); s && s->srv && use.dim == shader::TexDim::Tex2D && !use.array) {
        s->last_use = frame_;
        if (s->depth)
            swizzle = 0xB1A; // depth samples come back in .x
        const u32 w = tic.Width(), h = tic.Height();
        if (s->depth || w == 0 || h == 0 || w > s->width || h > s->height || (w == s->width && h == s->height)) {
            if (ID3D11ShaderResourceView* copy = r_.HazardSrv(s))
                return copy;
            return s->srv.Get();
        }
        // The texture covers only part of the target: sample a copy of exactly that region.
        if (s->sub_w != w || s->sub_h != h) {
            D3D11_TEXTURE2D_DESC td;
            s->tex->GetDesc(&td);
            td.Width = s->Scaled(w);
            td.Height = s->Scaled(h);
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            D3D11_SHADER_RESOURCE_VIEW_DESC sv;
            s->srv->GetDesc(&sv);
            s->sub_tex.Reset();
            s->sub_srv.Reset();
            if (FAILED(dev_->CreateTexture2D(&td, nullptr, &s->sub_tex)) ||
                FAILED(dev_->CreateShaderResourceView(s->sub_tex.Get(), &sv, &s->sub_srv)))
                return s->srv.Get();
            s->sub_w = w;
            s->sub_h = h;
            s->sub_serial = 0;
        }
        if (s->sub_serial != s->write_serial) { // only when the target was written since the last copy
            const D3D11_BOX box{0, 0, 0, s->Scaled(w), s->Scaled(h), 1};
            ctx_->CopySubresourceRegion(s->sub_tex.Get(), 0, 0, 0, 0, s->tex.Get(), 0, &box);
            s->sub_serial = s->write_serial;
        }
        return s->sub_srv.Get();
    }
    const u64 key = Hash64(tic.w.data(), 32) ^ (u64(use.dim) << 56) ^ (u64(use.array) << 60);
    auto it = textures_.find(key);
    if (it != textures_.end() && it->second->dma_dirty) {
        // (#8) rewritten by the GPU or DMA since the last decode (a video frame): decode into the same texture
        CachedTexture& t = *it->second;
        t.dma_dirty = false;
        for (auto& [a, s] : t.cpu_ranges)
            G().Mem().ConsumeDirty(a, s);
        dma_reuploads_++;
        reuploads++;
        g_texture_stats.reuploads.fetch_add(1, std::memory_order_relaxed);
        // all of it through the staging ring (UpdateRects, as for CPU rewrites), else decoded again by Upload
        static const std::vector<std::pair<u64, u64>> none;
        static const bool update_sub = std::getenv("MUA3_TEX_UPDATESUB") != nullptr;
        if (t.updatable && UpdateRects(tic, use, t, none, !update_sub, true))
            return t.srv.Get();
        if (auto* srv = Upload(tic, use, key, &t))
            return srv;
        Erase(it);
        auto* srv = Upload(tic, use, key, nullptr, true);
        return srv ? srv : Fallback(use);
    }
    if (it != textures_.end()) {
        // Re-decode if the CPU wrote to the guest copy since we uploaded it (e.g. glyph caches).
        bool dirty = false;
        for (auto& [a, s] : it->second->cpu_ranges)
            dirty |= G().Mem().ConsumeDirty(a, s);
        if (!dirty) {
            it->second->last_use = frame_;
            return it->second->srv.Get();
        }
        CachedTexture& t = *it->second;
        // Pages written, contents as uploaded: keep it (MUA3_TEX_NOHASH=1 re-decodes on every write as before).
        static const bool no_hash = std::getenv("MUA3_TEX_NOHASH") != nullptr;
        static thread_local std::vector<std::pair<u64, u64>> changed;
        changed.clear();
        if (!no_hash && !t.page_hash.empty()) {
            tracked_bytes += ChangedPages(t, changed);
            if (changed.empty()) {
                t.last_use = frame_;
                unchanged_rewrites++;
                g_texture_stats.unchanged.fetch_add(1, std::memory_order_relaxed);
                return t.srv.Get();
            }
        }
        reuploads++;
        g_texture_stats.reuploads.fetch_add(1, std::memory_order_relaxed);
        static std::map<u64, u32> logged; // (diagnostics) textures the CPU really rewrites
        if (logged.size() < 32 && logged[addr]++ == 0)
            LOG_INFO("Texture", "rewritten by the CPU: {:#x} fmt {:#x} {}x{}x{} type {} {} bytes", addr, tic.Format(),
                     tic.Width(), tic.Height(), tic.Depth(), tic.Type(), t.size);
        D3D11_RESOURCE_DIMENSION dim{};
        t.res->GetType(&dim);
        if (dim == D3D11_RESOURCE_DIMENSION_BUFFER) { // linear texel buffer: refresh the bytes in place
            D3D11_BUFFER_DESC bd;
            static_cast<ID3D11Buffer*>(t.res.Get())->GetDesc(&bd);
            static thread_local std::vector<u8> tmp;
            // only the changed bytes (pages already tracked and hashed again by ChangedPages), unless MUA3_TEX_FULL=1
            static const bool full_buffers = std::getenv("MUA3_TEX_FULL") != nullptr;
            if (!full_buffers && !changed.empty()) {
                u64 uploaded = 0;
                for (auto& [o, n] : changed) {
                    if (o >= bd.ByteWidth)
                        continue;
                    const u32 len = u32(std::min<u64>(n, bd.ByteWidth - o));
                    tmp.resize(len);
                    G().ReadGpuPartial(addr + o, tmp.data(), len);
                    const D3D11_BOX box{u32(o), 0, 0, u32(o) + len, 1, 1};
                    ctx_->UpdateSubresource(t.res.Get(), 0, &box, tmp.data(), 0, 0);
                    uploaded += len;
                }
                g_texture_stats.partial.fetch_add(1, std::memory_order_relaxed);
                g_texture_stats.reupload_bytes.fetch_add(uploaded, std::memory_order_relaxed);
                t.last_use = frame_;
                return t.srv.Get();
            }
            tmp.assign(bd.ByteWidth, 0);
            G().ReadGpuPartial(addr, tmp.data(), bd.ByteWidth);
            ctx_->UpdateSubresource(t.res.Get(), 0, nullptr, tmp.data(), 0, 0);
            g_texture_stats.reupload_bytes.fetch_add(bd.ByteWidth, std::memory_order_relaxed);
            HashPages(t);
            for (auto& [a, n] : t.cpu_ranges)
                G().Mem().TrackWrites(a, n), tracked_bytes += n;
            t.last_use = frame_;
            return t.srv.Get();
        }
        // Only the rewritten part (glyphs added to a 4 MB glyph cache), unless MUA3_TEX_FULL=1 (all of it, as before).
        // MUA3_TEX_STAGING=1: through reused staging textures and CopySubresourceRegion instead of UpdateSubresource
        // (GitHub #8: host memory grows on AMD while these are updated); with MUA3_TEX_FULL=1, all of it that way.
        static const bool full = std::getenv("MUA3_TEX_FULL") != nullptr;
        static const bool staging = std::getenv("MUA3_TEX_STAGING") != nullptr;
        if (t.updatable && (staging || (!full && !changed.empty())) &&
            UpdateRects(tic, use, t, changed, staging, full || changed.empty()))
            return t.srv.Get();
        if (t.updatable)
            if (auto* srv = Upload(tic, use, key, &t))
                return srv;
        // Rewritten once: re-create it updatable, so later rewrites update it in place.
        Erase(it);
        auto* srv = Upload(tic, use, key, nullptr, true);
        return srv ? srv : Fallback(use);
    }
    // Only in load-sized bursts (a level's textures arrive at once: 238-470 in a frame entering ST010): a hero swap or
    // an enemy spawn uploads far fewer and stays synchronous, so gameplay never draws a placeholder.
    // (#8) dropped by Invalidate before (guest memory rewritten under it): from now on updatable, decoded in place
    if (!invalidated_keys_.empty() && invalidated_keys_.erase(key)) {
        auto* srv = Upload(tic, use, key, nullptr, true);
        return srv ? srv : Fallback(use);
    }
    constexpr u64 kAsyncMinUploads = 96;
    if (budget_ns_ && use.dim == shader::TexDim::Tex2D && up_count_ >= kAsyncMinUploads &&
        up_ns_[0] + up_ns_[1] + up_ns_[2] + up_ns_[3] >= budget_ns_) {
        std::lock_guard lk(aq_mutex_);
        if (pending_.insert(key).second) {
            aq_.push_back({tic, use, key, inval_gen_});
            async_queued_++;
            aq_cv_.notify_one();
        }
        return Placeholder(use);
    }
    if (!pending_.empty()) { // (a job for it may be running: this frame has budget left, so do it now)
        std::lock_guard lk(aq_mutex_);
        if (pending_.count(key))
            return Placeholder(use);
    }
    auto* srv = Upload(tic, use, key);
    return srv ? srv : Fallback(use);
}

void TextureCache::AsyncWorker() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    for (;;) {
        AsyncJob job;
        {
            std::unique_lock lk(aq_mutex_);
            aq_cv_.wait(lk, [&] { return stop_ || !aq_.empty(); });
            if (stop_)
                return;
            job = aq_.front();
            aq_.pop_front();
        }
        std::unique_ptr<CachedTexture> tex;
        Upload(job.tic, job.use, job.key, nullptr, false, &tex);
        std::lock_guard lk(aq_mutex_);
        done_.push_back({job.key, job.gen, std::move(tex)});
        has_done_.store(true, std::memory_order_release);
    }
}

// Finished asynchronous uploads join the cache, unless guest memory under them was invalidated (DMA) after they
// were queued: then the next use uploads again.
void TextureCache::InstallAsync() {
    std::vector<AsyncDone> done;
    std::vector<std::pair<u64, std::pair<u64, u64>>> inval;
    {
        std::lock_guard lk(aq_mutex_);
        done.swap(done_);
        inval = inval_log_;
        has_done_.store(false, std::memory_order_relaxed);
        for (auto& d : done)
            pending_.erase(d.key);
    }
    for (auto& d : done) {
        bool stale = !d.tex || textures_.find(d.key) != textures_.end();
        for (auto& [gen, r] : inval)
            if (!stale && d.tex && gen > d.gen && r.first < d.tex->gpu_va + d.tex->size && d.tex->gpu_va < r.first + r.second)
                stale = true;
        if (stale) {
            async_dropped_++;
            continue;
        }
        d.tex->last_use = frame_;
        TrackPages(*d.tex, +1);
        textures_[d.key] = std::move(d.tex);
        async_installed_++;
    }
    std::lock_guard lk(aq_mutex_);
    if (pending_.empty())
        inval_log_.clear();
}

// Drawn until an asynchronous upload lands: transparent black (UI pieces stay hidden instead of grey boxes).
ID3D11ShaderResourceView* TextureCache::Placeholder(const shader::TextureUse& use) {
    if (use.dim != shader::TexDim::Tex2D || use.array)
        return Fallback(use);
    if (!placeholder_) {
        const u32 px = 0;
        const D3D11_SUBRESOURCE_DATA sd{&px, 4, 4};
        D3D11_TEXTURE2D_DESC td{1, 1, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D11_USAGE_IMMUTABLE, D3D11_BIND_SHADER_RESOURCE, 0, 0};
        ComPtr<ID3D11Texture2D> t;
        dev_->CreateTexture2D(&td, &sd, &t);
        dev_->CreateShaderResourceView(t.Get(), nullptr, &placeholder_);
    }
    return placeholder_.Get();
}

// A CPU-rewritten 2D texture (block linear, one level, updatable): only the rectangles under the changed pages are
// decoded and updated (UpdateSubresource with a box), instead of all of it. False if it does not apply: the caller
// uploads it whole.
bool TextureCache::UpdateRects(const TicEntry& tic, const shader::TextureUse& use, CachedTexture& t,
                               const std::vector<std::pair<u64, u64>>& changed, bool staging, bool whole) {
    // (diagnostics: why a rewritten texture is uploaded whole, once per texture)
    auto decline = [&](const char* why) {
        static std::set<u64> said;
        if (said.size() < 16 && said.insert(tic.Address()).second)
            LOG_INFO("Texture", "rewritten {:#x} fmt {:#x} {}x{} type {} header {}: uploaded whole ({})", tic.Address(),
                     tic.Format(), tic.Width(), tic.Height(), tic.Type(), tic.HeaderVersion(), why);
        return false;
    };
    if (use.dim != shader::TexDim::Tex2D || use.array || tic.IsBuffer())
        return decline("not a 2D texture");
    const u32 type = tic.Type();
    if (!(type == 7 || (type == 1 && tic.MaxMip() == 0)))
        return decline("mipmapped or not 2D");
    const TexFormatInfo fi = TicFormat(tic.Format(), tic.RType(), tic.Srgb());
    const u32 bpb = fi.bytes_per_block;
    if (fi.dxgi == DXGI_FORMAT_UNKNOWN || fi.astc || !bpb || 64 % bpb)
        return decline("format");
    D3D11_RESOURCE_DIMENSION dim{};
    t.res->GetType(&dim);
    if (dim != D3D11_RESOURCE_DIMENSION_TEXTURE2D)
        return decline("resource not 2D");
    const u32 w = tic.Width(), h = tic.Height();
    const u32 bw = (w + fi.block_w - 1) / fi.block_w, bh = (h + fi.block_h - 1) / fi.block_h;
    // block linear (header 3+) or pitch linear (rows tic.Pitch() bytes apart, as Upload reads them)
    const bool block_linear = tic.HeaderVersion() >= 3;
    const u32 src_pitch = tic.Pitch();
    u32 bhl = tic.BlockHeight();
    while (bhl > 0 && (8u << (bhl - 1)) >= bh)
        bhl--;
    if (!block_linear && src_pitch < bw * bpb)
        return decline("pitch below the row size");
    const u64 total = block_linear ? BlockLinearSize(bw, bh, 1, bpb, bhl, 0) : u64(src_pitch) * bh;
    if (total != t.size)
        return decline("size differs from the upload");
    u64 bytes = 0;
    for (auto& [o, n] : changed)
        bytes += n;
    if (bytes * 2 > total) { // most of it: one whole upload
        // (#8) through the reused staging ring, not UpdateSubresource: the AMD driver keeps a copy per whole-texture
        // UpdateSubresource while the GPU is behind (video planes every frame: memory +800 MB/s on an RX 6800).
        // MUA3_TEX_UPDATESUB=1: UpdateSubresource as before (A/B).
        static const bool update_sub = std::getenv("MUA3_TEX_UPDATESUB") != nullptr;
        if (!staging && update_sub)
            return decline("over half of it changed");
        staging = true;
        whole = true;
    }
    const u8* guest = G().GpuPtr(tic.Address(), size_t(total));
    if (!guest)
        return decline("guest memory not contiguous");
    static thread_local std::vector<BlockLinearRect> rects;
    if (whole) {
        rects.assign(1, BlockLinearRect{0, 0, bw, bh, 0});
    } else if (block_linear) {
        BlockLinearRects(bw, bh, bpb, bhl, changed.data(), changed.size(), rects);
    } else { // pitch linear: the rows under the changed bytes, whole width (`changed` is sorted, adjacent ranges merged)
        rects.clear();
        for (auto& [o, n] : changed) {
            const u32 y0 = u32(o / src_pitch), y1 = u32(std::min<u64>((o + n - 1) / src_pitch + 1, bh));
            if (y0 >= y1)
                continue;
            if (!rects.empty() && rects.back().y + rects.back().h >= y0)
                rects.back().h = std::max(rects.back().h, y1 - rects.back().y);
            else
                rects.push_back({0, y0, bw, y1 - y0, u64(y0) * src_pitch});
        }
    }
    // the rectangle's elements from guest memory into `dst` (rows `dst_pitch` bytes apart)
    auto decode = [&](const BlockLinearRect& r, u8* dst, size_t dst_pitch) {
        if (block_linear) {
            Deswizzle(guest + r.src, dst, r.w, r.h, 1, bpb, bhl, 0, u32(dst_pitch));
            return;
        }
        const size_t row = size_t(r.w) * bpb;
        for (u32 y = 0; y < r.h; y++)
            std::memcpy(dst + y * dst_pitch, guest + r.src + size_t(y) * src_pitch + size_t(r.x) * bpb, row);
    };
    auto box_of = [&](const BlockLinearRect& r) {
        return D3D11_BOX{r.x * fi.block_w, r.y * fi.block_h, 0, std::min(w, (r.x + r.w) * fi.block_w),
                         std::min(h, (r.y + r.h) * fi.block_h), 1};
    };
    u64 uploaded = 0;
    if (staging) {
        // decode straight into a staging texture of the same format and size, then copy the rectangles across
        D3D11_MAPPED_SUBRESOURCE m{};
        ID3D11Texture2D* st = MapStaging(static_cast<ID3D11Texture2D*>(t.res.Get()), m);
        if (!st)
            return false;
        for (const BlockLinearRect& r : rects) {
            u8* dst = static_cast<u8*>(m.pData) + size_t(r.y) * m.RowPitch + size_t(r.x) * bpb;
            decode(r, dst, m.RowPitch);
            uploaded += u64(r.w) * bpb * r.h;
        }
        ctx_->Unmap(st, 0);
        for (const BlockLinearRect& r : rects) {
            const D3D11_BOX box = box_of(r);
            ctx_->CopySubresourceRegion(t.res.Get(), 0, box.left, box.top, 0, st, 0, &box);
        }
        g_texture_stats.staged.fetch_add(1, std::memory_order_relaxed);
    } else {
        static thread_local std::vector<u8> lin;
        for (const BlockLinearRect& r : rects) {
            const u32 pitch = r.w * bpb;
            if (lin.size() < size_t(pitch) * r.h)
                lin.resize(size_t(pitch) * r.h);
            decode(r, lin.data(), pitch);
            const D3D11_BOX box = box_of(r);
            ctx_->UpdateSubresource(t.res.Get(), 0, &box, lin.data(), pitch, 0);
            uploaded += u64(pitch) * r.h;
        }
    }
    if (whole) { // (the whole texture: its pages hashed and tracked again, as Upload does)
        for (auto& [a, n] : t.cpu_ranges)
            G().Mem().TrackWrites(a, n), tracked_bytes += n;
        HashPages(t);
    }
    if (!whole)
        g_texture_stats.partial.fetch_add(1, std::memory_order_relaxed);
    g_texture_stats.reupload_bytes.fetch_add(uploaded, std::memory_order_relaxed);
    t.last_use = frame_;
    return true;
}

// A staging texture like `dst` (format, size), mapped for writing: the first of its ring the GPU is done with, a new
// one while the ring has fewer than kStagingRing, else the next in turn (waiting for it). Null if none could be made.
ID3D11Texture2D* TextureCache::MapStaging(ID3D11Texture2D* dst, D3D11_MAPPED_SUBRESOURCE& m) {
    constexpr size_t kStagingRing = 8;
    D3D11_TEXTURE2D_DESC d;
    dst->GetDesc(&d);
    StagingRing& ring = staging_[(u64(d.Format) << 48) ^ (u64(d.Width) << 24) ^ d.Height];
    const size_t n = ring.tex.size();
    for (size_t i = 0; i < n; i++) {
        const size_t k = (ring.next + i) % n;
        if (SUCCEEDED(ctx_->Map(ring.tex[k].Get(), 0, D3D11_MAP_WRITE, D3D11_MAP_FLAG_DO_NOT_WAIT, &m))) {
            ring.next = k + 1;
            return ring.tex[k].Get();
        }
    }
    if (n < kStagingRing) {
        d.MipLevels = d.ArraySize = 1;
        d.Usage = D3D11_USAGE_STAGING;
        d.BindFlags = 0;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        d.MiscFlags = 0;
        ComPtr<ID3D11Texture2D> s;
        if (FAILED(dev_->CreateTexture2D(&d, nullptr, &s)) || FAILED(ctx_->Map(s.Get(), 0, D3D11_MAP_WRITE, 0, &m))) {
            LOG_WARN("Texture", "staging texture {}x{} format {} could not be made", d.Width, d.Height, int(d.Format));
            return nullptr;
        }
        ring.tex.push_back(s);
        ring.next = n + 1;
        LOG_INFO("Texture", "staging texture {} of {} for {}x{} format {} (rewritten textures go through it)", n + 1, kStagingRing,
                 d.Width, d.Height, int(d.Format));
        return s.Get();
    }
    const size_t k = ring.next % n;
    if (FAILED(ctx_->Map(ring.tex[k].Get(), 0, D3D11_MAP_WRITE, 0, &m)))
        return nullptr;
    ring.next = k + 1;
    return ring.tex[k].Get();
}

ID3D11ShaderResourceView* TextureCache::Upload(const TicEntry& tic, const shader::TextureUse& use, u64 key, CachedTexture* reuse,
                                               bool updatable, std::unique_ptr<CachedTexture>* out) {
    const TexFormatInfo fi = TicFormat(tic.Format(), tic.RType(), tic.Srgb());
    uploads++;
    const bool async = out != nullptr;
    u64 t0 = NowNs();
    static std::mutex diag_mutex; // (workers upload too)
    {
        std::lock_guard dl(diag_mutex);
        static std::map<u64, u32> seen;
        if (seen.size() < 4096 && ++seen[tic.Address()] == 20)
            LOG_INFO("Texture", "uploaded 20 times: {:#x} fmt {:#x} {}x{}x{} type {} dim {} (frame {})", tic.Address(), tic.Format(),
                     tic.Width(), tic.Height(), tic.Depth(), tic.Type(), int(use.dim), frame_);
    }
    if (fi.dxgi == DXGI_FORMAT_UNKNOWN) {
        std::lock_guard dl(diag_mutex);
        static std::set<u32> warned;
        if (warned.insert(tic.Format()).second)
            LOG_WARN("Texture", "Unsupported texture format {:#x} (type {})", tic.Format(), tic.RType());
        return nullptr;
    }
    const u32 type = tic.Type();
    const u32 width = tic.Width(), height = (type == 0 || type == 4 || type == 6) ? 1 : tic.Height();
    u32 depth = 1, layers = 1;
    if (type == 2)
        depth = tic.Depth();
    else if (type == 4 || type == 5)
        layers = tic.Depth();
    else if (type == 3)
        layers = 6;
    else if (type == 8)
        layers = tic.Depth() * 6;
    const u32 mips = type == 7 ? 1 : std::min<u32>(tic.MaxMip() + 1, 16);
    const bool block_linear = tic.HeaderVersion() >= 3;
    const u32 bpb = fi.bytes_per_block;

    // Compute per-level layout
    struct Level {
        u32 w, h, d, bw, bh, bhl, bdl;
        u64 offset, size;
    };
    std::vector<Level> levels;
    auto pitch = [&](const Level& l) { return tic.IsBuffer() ? l.bw * bpb : tic.Pitch(); };
    u64 layer_size = 0;
    for (u32 m = 0; m < mips; m++) {
        Level l{};
        l.w = std::max(1u, width >> m);
        l.h = std::max(1u, height >> m);
        l.d = std::max(1u, depth >> m);
        l.bw = (l.w + fi.block_w - 1) / fi.block_w;
        l.bh = (l.h + fi.block_h - 1) / fi.block_h;
        l.bhl = tic.BlockHeight();
        l.bdl = tic.BlockDepth();
        while (l.bhl > 0 && (8u << (l.bhl - 1)) >= l.bh)
            l.bhl--;
        while (l.bdl > 0 && (1u << (l.bdl - 1)) >= l.d)
            l.bdl--;
        l.offset = layer_size;
        l.size = block_linear ? BlockLinearSize(l.bw, l.bh, l.d, bpb, l.bhl, l.bdl) : u64(pitch(l)) * l.bh * l.d;
        layer_size += l.size;
        levels.push_back(l);
    }
    if (block_linear && layers > 1)
        layer_size = common::AlignUp(layer_size, 512ull << (tic.BlockHeight() + tic.BlockDepth()));
    const u64 total = layer_size * layers;
    if (total == 0 || total > (512ull << 20)) {
        LOG_WARN("Texture", "Texture too large or empty ({} bytes)", total);
        return nullptr;
    }
    // A new texture: track CPU writes to its backing memory and hash its pages before reading it, so a write that
    // lands while it is decoded (on a worker, possibly) still marks it dirty.
    std::unique_ptr<CachedTexture> tex;
    if (!reuse) {
        tex = std::make_unique<CachedTexture>();
        for (u64 off = 0; off < total;) {
            u64 run = 0;
            const u64 cpu = G().As().TranslateRun(tic.Address() + off, run);
            if (!cpu || !run)
                break;
            const u64 n = std::min(run, total - off);
            tex->cpu_ranges.emplace_back(cpu, n);
            G().Mem().TrackWrites(cpu, n), tracked_bytes += n;
            off += n;
        }
        HashPages(*tex);
    }
    // Read guest memory in place when it is contiguous (textures rewritten every frame make this a hot path).
    const u8* guest = G().GpuPtr(tic.Address(), size_t(total));
    static thread_local std::vector<u8> guest_copy;
    if (!guest) {
        if (guest_copy.size() < total)
            guest_copy.resize(size_t(total));
        G().ReadGpu(tic.Address(), guest_copy.data(), size_t(total));
        guest = guest_copy.data();
    }

    // Decode every subresource into reused scratch buffers (no per-upload allocation or zero fill).
    struct Sub {
        u8* data;
        size_t size;
    };
    static thread_local std::vector<std::vector<u8>> pool;
    std::vector<Sub> sub;
    auto scratch = [&](size_t n) -> u8* {
        const size_t i = sub.size();
        if (pool.size() <= i)
            pool.resize(i + 1);
        if (pool[i].size() < n)
            pool[i].resize(n);
        sub.push_back({pool[i].data(), n});
        return pool[i].data();
    };
    std::vector<D3D11_SUBRESOURCE_DATA> init;
    const bool astc = fi.astc;
    for (u32 layer = 0; layer < layers; layer++) {
        for (u32 m = 0; m < mips; m++) {
            const Level& l = levels[m];
            const u8* src = guest + layer * layer_size + l.offset;
            const u32 row = l.bw * bpb;
            const size_t lin_size = size_t(row) * l.bh * l.d;
            static thread_local std::vector<u8> astc_lin;
            if (astc && astc_lin.size() < lin_size)
                astc_lin.resize(lin_size);
            u8* lin = astc ? astc_lin.data() : scratch(lin_size);
            if (block_linear)
                Deswizzle(src, lin, l.bw, l.bh, l.d, bpb, l.bhl, l.bdl, row);
            else
                for (u32 y = 0; y < l.bh * l.d; y++)
                    std::memcpy(lin + size_t(y) * row, src + size_t(y) * pitch(l), row);
            if (g_decoded_level_hook && bpb == 16 && !astc && l.d == 1 && (type == 1 || type == 7))
                g_decoded_level_hook(const_cast<u8*>(src), lin, l.bw, l.bh, block_linear, l.bhl, pitch(l));
            if (astc) {
                u8* rgba = scratch(size_t(l.w) * l.h * 4);
                DecodeAstc(lin, rgba, l.w, l.h, fi.block_w, fi.block_h);
                init.push_back({rgba, l.w * 4, l.w * l.h * 4});
            } else {
                init.push_back({lin, row, row * l.bh});
            }
        }
    }
    // Rewritten texture we already re-created as updatable: decode straight into it.
    if (reuse) {
        g_texture_stats.reupload_bytes.fetch_add(total, std::memory_order_relaxed);
        for (size_t i = 0; i < init.size(); i++)
            ctx_->UpdateSubresource(reuse->res.Get(), UINT(i), nullptr, init[i].pSysMem, init[i].SysMemPitch,
                                    init[i].SysMemSlicePitch);
        for (auto& [a, n] : reuse->cpu_ranges)
            G().Mem().TrackWrites(a, n), tracked_bytes += n;
        HashPages(*reuse);
        reuse->last_use = frame_;
        return reuse->srv.Get();
    }
    u64 t1 = NowNs();
    g_texture_stats.uploads.fetch_add(1, std::memory_order_relaxed);
    g_texture_stats.upload_bytes.fetch_add(total, std::memory_order_relaxed);
    if (!async) {
        up_ns_[0] += t1 - t0;
        up_count_++;
        up_bytes_ += total;
    }
    const D3D11_USAGE usage = updatable ? D3D11_USAGE_DEFAULT : D3D11_USAGE_IMMUTABLE;
    // The init array points into the thread-local scratch buffers (`sub`), valid until the next upload.
    for (size_t i = 0; i < init.size(); i++)
        init[i].pSysMem = sub[i].data;

    tex->key = key;
    tex->gpu_va = tic.Address();
    tex->size = total;
    tex->last_use = frame_;
    HRESULT hr;
    D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Format = fi.dxgi;
    if (use.dim == shader::TexDim::Buffer) {
        // Texel buffer: a typed D3D11 buffer over the first level's texels.
        const u32 elems = levels[0].bw;
        // DEFAULT usage: texel buffers the CPU rewrites every frame are updated in place (see Get).
        D3D11_BUFFER_DESC bd{u32(sub[0].size), D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, 0, 0};
        const D3D11_SUBRESOURCE_DATA bdata{sub[0].data, 0, 0};
        ComPtr<ID3D11Buffer> b;
        hr = dev_->CreateBuffer(&bd, &bdata, &b);
        tex->res = b;
        sv.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        sv.Buffer.FirstElement = 0;
        sv.Buffer.NumElements = elems;
    } else if (type == 2 && use.dim == shader::TexDim::Tex3D) {
        D3D11_TEXTURE3D_DESC td{width, height, depth, mips, fi.dxgi, usage, D3D11_BIND_SHADER_RESOURCE, 0, 0};
        ComPtr<ID3D11Texture3D> t;
        hr = dev_->CreateTexture3D(&td, init.data(), &t);
        tex->res = t;
        sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE3D;
        sv.Texture3D.MipLevels = mips;
    } else if (use.dim == shader::TexDim::Tex1D) {
        // 1D textures and texel buffers: the shader declares Texture1D(Array), which needs a 1D resource.
        if (width > D3D11_REQ_TEXTURE1D_U_DIMENSION)
            LOG_WARN("Texture", "1D texture of {} texels exceeds the D3D11 limit; truncated", width);
        D3D11_TEXTURE1D_DESC td{std::min<u32>(width, D3D11_REQ_TEXTURE1D_U_DIMENSION), mips, layers, fi.dxgi,
                                usage, D3D11_BIND_SHADER_RESOURCE, 0, 0};
        ComPtr<ID3D11Texture1D> t;
        hr = dev_->CreateTexture1D(&td, init.data(), &t);
        tex->res = t;
        if (use.array) {
            sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE1DARRAY;
            sv.Texture1DArray = {0, mips, 0, layers};
        } else {
            sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE1D;
            sv.Texture1D = {0, mips};
        }
    } else {
        const bool cube = use.dim == shader::TexDim::Cube && layers % 6 == 0;
        D3D11_TEXTURE2D_DESC td{width, height, mips, layers, fi.dxgi, {1, 0}, usage,
                                D3D11_BIND_SHADER_RESOURCE, 0, cube ? D3D11_RESOURCE_MISC_TEXTURECUBE : 0u};
        ComPtr<ID3D11Texture2D> t;
        hr = dev_->CreateTexture2D(&td, init.data(), &t);
        tex->res = t;
        if (cube) {
            if (use.array) {
                sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBEARRAY;
                sv.TextureCubeArray = {0, mips, 0, layers / 6};
            } else {
                sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE;
                sv.TextureCube = {0, mips};
            }
        } else if (use.array || layers > 1) {
            sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
            sv.Texture2DArray = {0, mips, 0, layers};
            if (!use.array)
                sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D, sv.Texture2D = {0, mips};
        } else {
            sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            sv.Texture2D = {0, mips};
        }
    }
    if (FAILED(hr) || !tex->res) {
        LOG_WARN("Texture", "CreateTexture failed {:#x} ({}x{} fmt {:#x} mips {} layers {})", u32(hr), width, height,
                 tic.Format(), mips, layers);
        return nullptr;
    }
    if (FAILED(dev_->CreateShaderResourceView(tex->res.Get(), &sv, &tex->srv)))
        return nullptr;
    if (!async)
        up_ns_[1] += NowNs() - t1;
    auto* srv = tex->srv.Get();
    tex->updatable = updatable;
    if (async) {
        *out = std::move(tex);
        return srv;
    }
    TrackPages(*tex, +1);
    textures_[key] = std::move(tex);
    return srv;
}

ID3D11SamplerState* TextureCache::GetSampler(const u32* tsc, bool compare) {
    const u64 key = Hash64(tsc, 32) ^ (compare ? 1ull : 0ull);
    auto it = samplers_.find(key);
    if (it != samplers_.end())
        return it->second.Get();
    auto wrap = [](u32 m) {
        switch (m) {
        case 0: return D3D11_TEXTURE_ADDRESS_WRAP;
        case 1: return D3D11_TEXTURE_ADDRESS_MIRROR;
        case 3: case 6: return D3D11_TEXTURE_ADDRESS_BORDER;
        case 5: case 7: return D3D11_TEXTURE_ADDRESS_MIRROR_ONCE;
        default: return D3D11_TEXTURE_ADDRESS_CLAMP;
        }
    };
    D3D11_SAMPLER_DESC d{};
    d.AddressU = wrap(tsc[0] & 7);
    d.AddressV = wrap((tsc[0] >> 3) & 7);
    d.AddressW = wrap((tsc[0] >> 6) & 7);
    const u32 aniso = (tsc[0] >> 20) & 7;
    const bool mag_lin = (tsc[1] & 3) == 2, min_lin = ((tsc[1] >> 4) & 3) == 2;
    const u32 mip = (tsc[1] >> 6) & 3;
    const bool mip_lin = mip == 3;
    if (aniso > 0 && min_lin && mag_lin) {
        d.Filter = D3D11_FILTER_ANISOTROPIC;
        d.MaxAnisotropy = std::min(16u, 1u << aniso);
    } else {
        u32 f = (min_lin ? 0x10 : 0) | (mag_lin ? 0x4 : 0) | (mip_lin ? 0x1 : 0);
        d.Filter = D3D11_FILTER(f);
    }
    if (compare) {
        d.Filter = D3D11_FILTER(d.Filter | 0x80);
        static const D3D11_COMPARISON_FUNC fn[] = {D3D11_COMPARISON_NEVER, D3D11_COMPARISON_LESS, D3D11_COMPARISON_EQUAL,
                                                   D3D11_COMPARISON_LESS_EQUAL, D3D11_COMPARISON_GREATER,
                                                   D3D11_COMPARISON_NOT_EQUAL, D3D11_COMPARISON_GREATER_EQUAL,
                                                   D3D11_COMPARISON_ALWAYS};
        d.ComparisonFunc = fn[(tsc[0] >> 10) & 7];
    } else {
        d.ComparisonFunc = D3D11_COMPARISON_NEVER;
    }
    const s32 bias = s32(((tsc[1] >> 12) & 0x1FFF) << 19) >> 19;
    d.MipLODBias = bias / 256.0f;
    d.MinLOD = (tsc[2] & 0xFFF) / 256.0f;
    d.MaxLOD = mip == 1 ? 0.0f : ((tsc[2] >> 12) & 0xFFF) / 256.0f;
    std::memcpy(d.BorderColor, &tsc[4], 16);
    ComPtr<ID3D11SamplerState> s;
    dev_->CreateSamplerState(&d, &s);
    samplers_[key] = s;
    return s.Get();
}

} // namespace gpu
