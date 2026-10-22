#include "gpu/gpu.h"

#include <chrono>

#include <windows.h>

#include "core/memory.h"
#include "gpu/renderer.h"

namespace gpu {

static Gpu* g_gpu = nullptr;
Gpu& G() { return *g_gpu; }
void Init(core::Memory& mem) { g_gpu = new Gpu(mem); }

// ---------------------------------------------------------------- AddressSpace
void AddressSpace::Map(u64 gpu_va, u64 cpu_va, u64 size) {
    std::lock_guard lk(mutex_);
    auto it = maps_.lower_bound(gpu_va);
    if (it != maps_.begin()) {
        auto p = std::prev(it);
        if (p->first + p->second.size > gpu_va) {
            const u64 end = p->first + p->second.size;
            p->second.size = gpu_va - p->first;
            if (end > gpu_va + size)
                maps_.emplace(gpu_va + size,
                              Mapping{end - (gpu_va + size),
                                      p->second.cpu_va ? p->second.cpu_va + (gpu_va + size - p->first) : 0});
        }
    }
    it = maps_.lower_bound(gpu_va);
    while (it != maps_.end() && it->first < gpu_va + size) {
        if (it->first + it->second.size > gpu_va + size) {
            Mapping tail{it->first + it->second.size - (gpu_va + size),
                         it->second.cpu_va ? it->second.cpu_va + (gpu_va + size - it->first) : 0};
            maps_.emplace(gpu_va + size, tail);
        }
        it = maps_.erase(it);
    }
    maps_[gpu_va] = {size, cpu_va};
}

void AddressSpace::Unmap(u64 gpu_va, u64 size) { Map(gpu_va, 0, size); }

u64 AddressSpace::Allocate(u64 size, u64 align) {
    std::lock_guard lk(mutex_);
    next_ = common::AlignUp(next_, std::max<u64>(align, 0x1000));
    const u64 va = next_;
    next_ += common::AlignUp(size, 0x1000);
    return va;
}

void AddressSpace::Reserve(u64 gpu_va, u64 size) {
    std::lock_guard lk(mutex_);
    if (gpu_va + size > next_ && gpu_va < 0x0400000000ull + 0x100000000000ull)
        next_ = std::max(next_, gpu_va + size);
}

u64 AddressSpace::TranslateRun(u64 gpu_va, u64& run) const {
    std::lock_guard lk(mutex_);
    auto it = maps_.upper_bound(gpu_va);
    if (it == maps_.begin())
        return run = 0;
    --it;
    if (gpu_va >= it->first + it->second.size || !it->second.cpu_va)
        return run = 0;
    run = it->first + it->second.size - gpu_va;
    return it->second.cpu_va + (gpu_va - it->first);
}

u64 AddressSpace::Translate(u64 gpu_va) const {
    std::lock_guard lk(mutex_);
    auto it = maps_.upper_bound(gpu_va);
    if (it == maps_.begin())
        return 0;
    --it;
    if (gpu_va >= it->first + it->second.size || !it->second.cpu_va)
        return 0;
    return it->second.cpu_va + (gpu_va - it->first);
}

// ---------------------------------------------------------------- Gpu
Gpu::Gpu(core::Memory& mem) : mem_(mem), m3d_(*this) {
    for (auto& r : regs_)
        r.assign(0x1000, 0);
    thread_ = std::thread([this] { ThreadMain(); });
}

Gpu::~Gpu() {
    running_ = false;
    queue_cv_.notify_all();
    if (thread_.joinable())
        thread_.join();
}

void Gpu::SetRenderer(Renderer* r) {
    WaitIdle();
    renderer_ = r;
}

u64 Gpu::NowNs() const {
    return u64(std::chrono::duration_cast<std::chrono::nanoseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
                   .count());
}

bool Gpu::ReadGpu(u64 gpu_va, void* dst, size_t size) {
    u8* d = static_cast<u8*>(dst);
    while (size) {
        u64 run = 0;
        const u64 cpu = as_.TranslateRun(gpu_va, run);
        const size_t n = size_t(std::min<u64>(size, run));
        if (!cpu || !n || !mem_.IsValid(cpu, n)) {
            std::memset(d, 0, size);
            return false;
        }
        mem_.ReadBlock(cpu, d, n);
        d += n, gpu_va += n, size -= n;
    }
    return true;
}

u8* Gpu::GpuPtr(u64 gpu_va, size_t size) {
    u64 run = 0;
    const u64 cpu = as_.TranslateRun(gpu_va, run);
    if (!cpu || run < size || !mem_.IsValid(cpu, size))
        return nullptr;
    return mem_.Ptr(cpu);
}

size_t Gpu::ReadGpuPartial(u64 gpu_va, void* dst, size_t size) {
    u8* d = static_cast<u8*>(dst);
    size_t done = 0;
    while (done < size) {
        const u64 cpu = as_.Translate(gpu_va);
        const size_t n = std::min<size_t>(size - done, 0x1000 - (gpu_va & 0xFFF));
        if (!cpu || !mem_.IsValid(cpu, n))
            break;
        mem_.ReadBlock(cpu, d + done, n);
        done += n, gpu_va += n;
    }
    return done;
}

bool Gpu::WriteGpu(u64 gpu_va, const void* src, size_t size) {
    const u8* s = static_cast<const u8*>(src);
    while (size) {
        u64 run = 0;
        const u64 cpu = as_.TranslateRun(gpu_va, run);
        const size_t n = size_t(std::min<u64>(size, run));
        if (!cpu || !n || !mem_.IsValid(cpu, n))
            return false;
        mem_.WriteBlock(cpu, s, n);
        s += n, gpu_va += n, size -= n;
    }
    return true;
}

u32 Gpu::ReserveSyncpoint(u32 id, u32 count) {
    if (id >= kNumSyncpoints)
        return 0;
    return syncpoint_max_[id].fetch_add(count) + count;
}

void Gpu::IncrementSyncpoint(u32 id, u32 count) {
    if (id >= kNumSyncpoints)
        return;
    {
        std::lock_guard lk(sync_mutex_);
        syncpoints_[id] += count;
        if (s32(syncpoints_[id] - syncpoint_max_[id]) > 0)
            syncpoint_max_[id] = syncpoints_[id].load();
    }
    sync_cv_.notify_all();
}

bool Gpu::WaitSyncpoint(u32 id, u32 value, s64 timeout_us) {
    if (id >= kNumSyncpoints)
        return true;
    std::unique_lock lk(sync_mutex_);
    auto done = [&] { return s32(syncpoints_[id] - value) >= 0; };
    if (timeout_us < 0) {
        sync_cv_.wait(lk, done);
        return true;
    }
    return sync_cv_.wait_for(lk, std::chrono::microseconds(timeout_us), done);
}

void Gpu::Submit(std::vector<u64> entries, u32 syncpt_id, u32 syncpt_incr) {
    submits++;
    {
        std::lock_guard lk(queue_mutex_);
        Job j{Job::Commands};
        j.entries = std::move(entries);
        j.syncpt_id = syncpt_id;
        j.syncpt_incr = syncpt_incr;
        queue_.push_back(std::move(j));
    }
    queue_cv_.notify_one();
}

void Gpu::QueuePresent(const PresentInfo& p) {
    {
        std::lock_guard lk(queue_mutex_);
        Job j{Job::Present};
        j.present = p;
        queue_.push_back(std::move(j));
    }
    queue_cv_.notify_one();
}

void Gpu::WaitIdle() {
    std::unique_lock lk(queue_mutex_);
    idle_cv_.wait(lk, [&] { return queue_.empty() && !busy_; });
}

void Gpu::ThreadMain() {
    SetThreadDescription(GetCurrentThread(), L"GPU");
    for (;;) {
        Job job;
        {
            std::unique_lock lk(queue_mutex_);
            busy_ = false;
            idle_cv_.notify_all();
            queue_cv_.wait(lk, [&] { return !queue_.empty() || !running_; });
            if (!running_)
                return;
            job = std::move(queue_.front());
            queue_.pop_front();
            busy_ = true;
        }
        switch (job.kind) {
        case Job::Commands: {
            const u64 t0 = NowNs();
            RunCommands(job.entries);
            busy_ns += NowNs() - t0;
            if (renderer_)
                renderer_->FlushCommands();
            if (job.syncpt_incr)
                IncrementSyncpoint(job.syncpt_id, job.syncpt_incr);
            break;
        }
        case Job::Present:
            if (renderer_)
                renderer_->Present(job.present);
            presents++;
            if (on_present)
                on_present(job.present);
            break;
        default:
            break;
        }
    }
}

void Gpu::RunCommands(const std::vector<u64>& entries) {
    for (u64 e : entries) {
        const u64 va = e & 0xFFFFFFFFFCull;
        const u32 words = u32((e >> 42) & 0x1FFFFF);
        if (words)
            ProcessPushbuffer(va, words);
    }
    FlushMacro();
}

void Gpu::ProcessPushbuffer(u64 gpu_va, u32 words) {
    std::vector<u32> w(words);
    if (!ReadGpu(gpu_va, w.data(), size_t(words) * 4)) {
        LOG_WARN("GPU", "Pushbuffer at unmapped GPU VA {:#x}", gpu_va);
        return;
    }
    for (u32 i = 0; i < words;) {
        const u32 hdr = w[i++];
        const u32 mode = hdr >> 29;
        const u32 subch = (hdr >> 13) & 7;
        const u32 method = hdr & 0x1FFF;
        const u32 count = (hdr >> 16) & 0x1FFF;
        switch (mode) {
        case 1: // increasing
            for (u32 k = 0; k < count && i < words; k++)
                CallMethod(subch, method + k, w[i++], k + 1 == count);
            break;
        case 3: // non-increasing
            for (u32 k = 0; k < count && i < words; k++)
                CallMethod(subch, method, w[i++], k + 1 == count);
            break;
        case 4: // inline immediate
            CallMethod(subch, method, count, true);
            break;
        case 5: // increase once
            for (u32 k = 0; k < count && i < words; k++)
                CallMethod(subch, method + (k ? 1 : 0), w[i++], k + 1 == count);
            break;
        default:
            break;
        }
    }
}

void Gpu::FlushMacro() {
    if (macro_index_ < 0)
        return;
    const u32 idx = u32(macro_index_);
    macro_index_ = -1;
    auto params = std::move(macro_params_);
    macro_params_.clear();
    m3d_.CallMacro(idx, params);
}

namespace {
inline u32 GobOffset(u32 x, u32 y) {
    return ((x % 64) / 32) * 256 + ((y % 8) / 2) * 64 + ((x % 32) / 16) * 32 + (y % 2) * 16 + (x % 16);
}
// Byte offset of (x bytes, y, z) in a block-linear surface `width_bytes` wide and `height` rows tall.
inline u64 BlockLinearOffset(u32 x, u32 y, u32 z, u32 width_bytes, u32 height, u32 bh, u32 bd) {
    const u32 gobs_x = (width_bytes + 63) / 64;
    const u32 rows_per_block = 8u << bh;
    const u32 blocks_y = (height + rows_per_block - 1) / rows_per_block;
    const u64 block_size = 512ull << (bh + bd);
    const u64 block = (u64(z >> bd) * blocks_y + y / rows_per_block) * gobs_x + x / 64;
    const u32 gob_in_block = (z & ((1u << bd) - 1)) * (1u << bh) + (y % rows_per_block) / 8;
    return block * block_size + u64(gob_in_block) * 512 + GobOffset(x, y);
}
} // namespace

// Maxwell DMA engine (0xB0B5): pitch/block-linear copies used by NVN for texture and buffer uploads.
void Gpu::RunDma(const u32* r, u32 launch) {
    const u64 src = (u64(r[0x100] & 0xFF) << 32) | r[0x101];
    const u64 dst = (u64(r[0x102] & 0xFF) << 32) | r[0x103];
    const u32 pitch_in = r[0x104], pitch_out = r[0x105];
    u32 len = r[0x106];
    const bool multi = launch & (1u << 9);
    const bool remap = launch & (1u << 10);
    const bool src_pitch = launch & (1u << 7), dst_pitch = launch & (1u << 8);
    const u32 lines = multi ? std::max<u32>(r[0x107], 1) : 1;
    if (!len)
        return;

    u32 bpp = 1; // bytes per element (DMA works in bytes unless remapping)
    if (remap) {
        const u32 rm = r[0x1C2];
        const u32 comp_size = ((rm >> 16) & 3) + 1;
        const u32 dst_comps = ((rm >> 24) & 3) + 1;
        bpp = comp_size * dst_comps;
        // Constant fills (memset): every destination component comes from const A/B.
        bool all_const = true;
        for (u32 c = 0; c < dst_comps; c++) {
            const u32 sel = (rm >> (c * 4)) & 7;
            if (sel != 4 && sel != 5)
                all_const = false;
        }
        if (all_const && dst_pitch) {
            std::vector<u8> elem(bpp);
            for (u32 c = 0; c < dst_comps; c++) {
                const u32 sel = (rm >> (c * 4)) & 7;
                const u32 v = sel == 4 ? r[0x1C0] : r[0x1C1];
                std::memcpy(&elem[c * comp_size], &v, comp_size);
            }
            std::vector<u8> line(size_t(len) * bpp);
            for (size_t i = 0; i < line.size(); i += bpp)
                std::memcpy(&line[i], elem.data(), bpp);
            for (u32 l = 0; l < lines; l++)
                WriteGpu(dst + u64(l) * pitch_out, line.data(), line.size());
            if (renderer_)
                renderer_->InvalidateRegion(dst, u64(lines) * std::max<u32>(pitch_out, u32(line.size())));
            return;
        }
        len *= bpp;
    }

    if (src_pitch && dst_pitch) {
        std::vector<u8> buf(len);
        for (u32 l = 0; l < lines; l++) {
            ReadGpu(src + u64(l) * pitch_in, buf.data(), len);
            WriteGpu(dst + u64(l) * pitch_out, buf.data(), len);
        }
        if (renderer_)
            renderer_->InvalidateRegion(dst, u64(lines) * (multi ? pitch_out : len));
        return;
    }

    static const bool no_bl = std::getenv("MUA3_NO_BLDMA") != nullptr; // test switch
    if (no_bl)
        return;
    // Surface parameters: block size (height bits 4-7, depth 8-11), width (bytes), height, depth, layer, origin.
    auto params = [&](u32 base) {
        struct P {
            u32 bh, bd, width, height, depth, layer, ox, oy;
        } p{(r[base] >> 4) & 0xF, (r[base] >> 8) & 0xF, r[base + 1], r[base + 2], std::max<u32>(r[base + 3], 1),
            r[base + 4], r[base + 5] & 0xFFFF, r[base + 5] >> 16};
        return p;
    };
    if (src_pitch && !dst_pitch) { // linear -> block-linear (texture upload)
        const auto d = params(0x1C3);
        const u32 width_bytes = d.width * (remap ? bpp : 1);
        const u64 surf_size = u64(((width_bytes + 63) / 64)) * ((d.height + (8u << d.bh) - 1) / (8u << d.bh)) *
                              ((d.depth + (1u << d.bd) - 1) >> d.bd) * (512ull << (d.bh + d.bd));
        // Write straight into guest memory when the surface is mapped contiguously (the common case).
        std::vector<u8> surf_copy;
        u8* surf = GpuPtr(dst, size_t(surf_size));
        if (!surf) {
            surf_copy.resize(size_t(surf_size));
            ReadGpu(dst, surf_copy.data(), surf_copy.size());
            surf = surf_copy.data();
        }
        std::vector<u8> line(len);
        const u32 ox = d.ox * (remap ? bpp : 1);
        for (u32 l = 0; l < lines; l++) {
            const u8* sl = GpuPtr(src + u64(l) * pitch_in, len);
            if (!sl) {
                ReadGpu(src + u64(l) * pitch_in, line.data(), len);
                sl = line.data();
            }
            for (u32 x = 0; x < len;) {
                const u32 n = std::min<u32>(16 - ((ox + x) & 15), len - x);
                const u64 off = BlockLinearOffset(ox + x, d.oy + l, d.layer, width_bytes, d.height, d.bh, d.bd);
                if (off + n <= surf_size)
                    std::memcpy(surf + off, sl + x, n);
                x += n;
            }
        }
        if (!surf_copy.empty())
            WriteGpu(dst, surf_copy.data(), surf_copy.size());
        if (renderer_)
            renderer_->InvalidateRegion(dst, surf_size);
        return;
    }
    if (!src_pitch && dst_pitch) { // block-linear -> linear (readback / copies)
        const auto s = params(0x1CA);
        const u32 width_bytes = s.width * (remap ? bpp : 1);
        const u64 surf_size = u64(((width_bytes + 63) / 64)) * ((s.height + (8u << s.bh) - 1) / (8u << s.bh)) *
                              ((s.depth + (1u << s.bd) - 1) >> s.bd) * (512ull << (s.bh + s.bd));
        std::vector<u8> surf_copy;
        const u8* surf = GpuPtr(src, size_t(surf_size));
        if (!surf) {
            surf_copy.resize(size_t(surf_size));
            ReadGpu(src, surf_copy.data(), surf_copy.size());
            surf = surf_copy.data();
        }
        std::vector<u8> line(len);
        const u32 ox = s.ox * (remap ? bpp : 1);
        for (u32 l = 0; l < lines; l++) {
            for (u32 x = 0; x < len;) {
                const u32 n = std::min<u32>(16 - ((ox + x) & 15), len - x);
                const u64 off = BlockLinearOffset(ox + x, s.oy + l, s.layer, width_bytes, s.height, s.bh, s.bd);
                if (off + n <= surf_size)
                    std::memcpy(&line[x], surf + off, n);
                x += n;
            }
            WriteGpu(dst + u64(l) * pitch_out, line.data(), len);
        }
        if (renderer_)
            renderer_->InvalidateRegion(dst, u64(lines) * pitch_out);
        return;
    }
    LOG_TRACE("GPU", "DMA block-linear -> block-linear copy not handled");
}

void Gpu::CallMethod(u32 subch, u32 method, u32 arg, bool last_in_batch) {
    methods++;
    if (method < 0x40) { // GPFIFO / puller methods (any subchannel)
        u32* r = regs_[0].data();
        r[method] = arg;
        switch (method) {
        case 0x0:
            bound_class_[subch] = arg & 0xFFFF;
            break;
        case 0x7: { // SEMAPHORED
            const u64 addr = (u64(r[0x4] & 0xFF) << 32) | r[0x5];
            if ((arg & 7) == 2) { // release
                WriteGpu(addr, &r[0x6], 4);
                if (!(arg & (1u << 24))) {
                    const u64 ts = NowNs();
                    WriteGpu(addr + 8, &ts, 8);
                }
            }
            break;
        }
        case 0x1D: // SYNCPOINTB
            if (arg & 1)
                IncrementSyncpoint((arg >> 8) & 0xFF);
            break;
        default:
            break;
        }
        return;
    }
    const u32 cls = bound_class_[subch];
    if (cls == 0xB197) {
        if (method >= reg3d::kMacroCallBase) {
            const s32 idx = s32((method - reg3d::kMacroCallBase) >> 1);
            if (!(method & 1)) {
                FlushMacro();
                macro_index_ = idx;
                macro_params_.assign(1, arg);
            } else {
                macro_params_.push_back(arg);
            }
            if (last_in_batch)
                FlushMacro();
            return;
        }
        FlushMacro();
        m3d_.CallMethod(method, arg);
        return;
    }

    u32* r = regs_[subch].data();
    if (method < 0x1000)
        r[method] = arg;

    // Inline-to-memory (stand-alone A140 / compute's copy)
    if (method == 0x6C) {
        r[0xF00] = 0;
        return;
    }
    if (method == 0x6D) {
        const u64 dst = (u64(r[0x62] & 0xFF) << 32) | r[0x63];
        const u32 line_len = r[0x60], lines = std::max<u32>(r[0x61], 1);
        const u32 cur = r[0xF00];
        if (cur < line_len * lines) {
            const u32 line = line_len ? cur / line_len : 0;
            const u32 col = line_len ? cur % line_len : cur;
            const u64 a = dst + u64(line) * r[0x64] + col;
            WriteGpu(a, &arg, std::min<u32>(4, line_len * lines - cur));
            if (renderer_)
                renderer_->InvalidateRegion(a, 4);
        }
        r[0xF00] = cur + 4;
        return;
    }

    if (method == 0xB2 && (cls == 0xB1C0 || cls == 0x902D)) { // other engines' syncpoint increments
        IncrementSyncpoint(arg & 0xFFFF);
        return;
    }
    if (cls == 0xB1C0) { // compute: report semaphore (dispatch not yet supported)
        if (method == 0x6C3) {
            const u64 addr = (u64(r[0x6C0] & 0xFF) << 32) | r[0x6C1];
            if ((arg & 3) == 0) {
                if (arg & (1u << 28)) {
                    WriteGpu(addr, &r[0x6C2], 4);
                } else {
                    const u64 v[2] = {r[0x6C2], NowNs()};
                    WriteGpu(addr, v, 16);
                }
            }
        }
        return;
    }

    if (cls == 0x902D) { // Fermi 2D
        if (method == 0x22F && renderer_) // PIXELS_FROM_MEMORY_SRC_Y0_INT (last register written) -> blit
            renderer_->Blit2D(r);
        return;
    }

    if (cls == 0xB0B5) { // DMA copy
        if (method == 0xC0) { // LAUNCH_DMA
            RunDma(r, arg);
            const u32 sem = (arg >> 3) & 3;
            if (sem) {
                const u64 addr = (u64(r[0x90] & 0xFF) << 32) | r[0x91];
                WriteGpu(addr, &r[0x92], 4);
                if (sem == 2) {
                    const u64 ts = NowNs();
                    WriteGpu(addr + 8, &ts, 8);
                }
            }
        }
        return;
    }
}

} // namespace gpu
