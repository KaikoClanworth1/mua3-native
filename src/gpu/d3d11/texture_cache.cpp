#include "gpu/d3d11/texture_cache.h"

#include <chrono>
#include <windows.h>
#include <map>
#include <set>

#include "core/memory.h"
#include "gpu/d3d11/renderer_d3d11.h"
#include "gpu/d3d11/shader_cache.h"
#include "gpu/formats.h"
#include "gpu/gpu.h"

namespace gpu {

TextureCache::TextureCache(ID3D11Device* dev, ID3D11DeviceContext* ctx, D3D11Renderer& r)
    : dev_(dev), ctx_(ctx), r_(r) {
    const char* b = std::getenv("MUA3_TEX_BUDGET_MS");
    budget_ns_ = u64(b ? std::atof(b) : 8.0) * 1'000'000;
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
        if (t.gpu_va < gpu_va + size && gpu_va < t.gpu_va + t.size)
            Erase(it);
        else
            ++it;
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
// True if a dirty page's contents differ from when the texture was decoded (pages are re-hashed: call it once).
static bool DirtyPagesChanged(CachedTexture& t) {
    bool changed = false;
    ForEachPage(t, [&](size_t k, u64 cpu, u64 n) {
        if (changed || !G().Mem().ConsumeDirty(cpu, n))
            return;
        changed = k >= t.page_hash.size() || ContentHash(G().Mem().Ptr(cpu), size_t(n)) != t.page_hash[k];
    });
    return changed;
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
        if (!no_hash && !t.page_hash.empty() && !DirtyPagesChanged(t)) {
            for (auto& [a, n] : t.cpu_ranges)
                G().Mem().TrackWrites(a, n), tracked_bytes += n;
            t.last_use = frame_;
            unchanged_rewrites++;
            return t.srv.Get();
        }
        reuploads++;
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
            tmp.assign(bd.ByteWidth, 0);
            G().ReadGpuPartial(addr, tmp.data(), bd.ByteWidth);
            ctx_->UpdateSubresource(t.res.Get(), 0, nullptr, tmp.data(), 0, 0);
            HashPages(t);
            for (auto& [a, n] : t.cpu_ranges)
                G().Mem().TrackWrites(a, n), tracked_bytes += n;
            t.last_use = frame_;
            return t.srv.Get();
        }
        if (t.updatable)
            if (auto* srv = Upload(tic, use, key, &t))
                return srv;
        // Rewritten once: re-create it updatable, so later rewrites update it in place.
        Erase(it);
        auto* srv = Upload(tic, use, key, nullptr, true);
        return srv ? srv : Fallback(use);
    }
    if (budget_ns_ && use.dim == shader::TexDim::Tex2D &&
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
