#include "gpu/gpu.h"

#include <chrono>

#include "core/memory.h"

namespace gpu {

static Gpu* g_gpu = nullptr;
Gpu& G() { return *g_gpu; }
void Init(core::Memory& mem) { g_gpu = new Gpu(mem); }

// ---------------------------------------------------------------- AddressSpace
void AddressSpace::Map(u64 gpu_va, u64 cpu_va, u64 size) {
    std::lock_guard lk(mutex_);
    // Drop overlapping mappings (remaps are common).
    auto it = maps_.lower_bound(gpu_va);
    if (it != maps_.begin()) {
        auto p = std::prev(it);
        if (p->first + p->second.size > gpu_va)
            p->second.size = gpu_va - p->first;
    }
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
Gpu::Gpu(core::Memory& mem) : mem_(mem) {}

u64 Gpu::NowNs() const {
    return u64(std::chrono::duration_cast<std::chrono::nanoseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
                   .count());
}

u32 Gpu::IncrementSyncpoint(u32 id, u32 count) {
    if (id >= kNumSyncpoints)
        return 0;
    return syncpoints_[id].fetch_add(count) + count;
}

void Gpu::SetSyncpoint(u32 id, u32 v) {
    if (id < kNumSyncpoints)
        syncpoints_[id] = v;
}

void Gpu::Submit(const u64* entries, u32 count) {
    std::lock_guard lk(submit_mutex_);
    submits++;
    for (u32 i = 0; i < count; i++) {
        const u64 e = entries[i];
        const u64 va = e & 0xFFFFFFFFFCull;
        const u32 words = u32((e >> 42) & 0x1FFFFF);
        if (words)
            ProcessPushbuffer(va, words);
    }
}

void Gpu::ProcessPushbuffer(u64 gpu_va, u32 words) {
    // Translate page by page into a local copy.
    std::vector<u32> w(words);
    for (u32 i = 0; i < words;) {
        const u64 va = gpu_va + u64(i) * 4;
        const u64 cpu = as_.Translate(va);
        const u32 in_page = u32((0x1000 - (va & 0xFFF)) / 4);
        const u32 n = std::min(in_page, words - i);
        if (!cpu || !mem_.IsValid(cpu, n * 4)) {
            LOG_WARN("GPU", "Pushbuffer at unmapped GPU VA {:#x}", va);
            return;
        }
        mem_.ReadBlock(cpu, &w[i], n * 4);
        i += n;
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
                CallMethod(subch, method + k, w[i++]);
            break;
        case 3: // non-increasing
            for (u32 k = 0; k < count && i < words; k++)
                CallMethod(subch, method, w[i++]);
            break;
        case 4: // inline immediate
            CallMethod(subch, method, count);
            break;
        case 5: // increase once
            for (u32 k = 0; k < count && i < words; k++)
                CallMethod(subch, method + (k ? 1 : 0), w[i++]);
            break;
        default:
            break; // nop / unsupported
        }
    }
}

void Gpu::CallMethod(u32 subch, u32 method, u32 arg) {
    methods++;
    if (method >= 0x1000)
        return;
    u32* r = regs_[subch];
    r[method] = arg;
    auto write32 = [&](u64 gva, u32 v) {
        const u64 cpu = as_.Translate(gva);
        if (cpu && mem_.IsValid(cpu, 4))
            mem_.Write<u32>(cpu, v);
    };
    auto write64 = [&](u64 gva, u64 v) {
        const u64 cpu = as_.Translate(gva);
        if (cpu && mem_.IsValid(cpu, 8))
            mem_.Write<u64>(cpu, v);
    };

    if (method < 0x40) { // GPFIFO / puller methods
        switch (method) {
        case 0x0:
            bound_class_[subch] = arg & 0xFFFF;
            break;
        case 0x7: { // SEMAPHORED
            const u64 addr = (u64(r[0x4] & 0xFF) << 32) | r[0x5];
            if ((arg & 7) == 2) { // release
                write32(addr, r[0x6]);
                if (!(arg & (1u << 24)))
                    write64(addr + 8, NowNs());
            }
            break;
        }
        case 0x1D: // SYNCPOINTB
            if (arg & 1)
                IncrementSyncpoint((arg >> 8) & 0xFF);
            break;
        default: break;
        }
        return;
    }

    const u32 cls = bound_class_[subch];
    // Inline-to-memory (stand-alone A140 class, and the same registers inside 3D/compute)
    if (method == 0x6C) {
        r[0xF00] = 0; // write cursor
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
            const u64 cpu = as_.Translate(a);
            const u32 n = std::min<u32>(4, line_len * lines - cur);
            if (cpu && mem_.IsValid(cpu, n))
                mem_.WriteBlock(cpu, &arg, n);
        }
        r[0xF00] = cur + 4;
        return;
    }

    if (cls == 0xB197 || cls == 0xB1C0) { // 3D / compute report semaphore
        if (method == 0x6C3) {
            const u64 addr = (u64(r[0x6C0] & 0xFF) << 32) | r[0x6C1];
            const u32 op = arg & 3;
            if (op == 0 || op == 2) {
                const u32 value = op == 0 ? r[0x6C2] : 0;
                if (arg & (1u << 28)) {
                    write32(addr, value);
                } else {
                    write64(addr, value);
                    write64(addr + 8, NowNs());
                }
            }
        }
        return;
    }

    if (cls == 0xB0B5) { // DMA copy
        if (method == 0xC0) { // LAUNCH_DMA
            const u64 src = (u64(r[0x40] & 0xFF) << 32) | r[0x41];
            const u64 dst = (u64(r[0x42] & 0xFF) << 32) | r[0x43];
            const u32 len = r[0x46];
            const bool multi = arg & (1u << 9);
            const bool src_pitch = arg & (1u << 7), dst_pitch = arg & (1u << 8);
            const u32 lines = multi ? std::max<u32>(r[0x47], 1) : 1;
            if (src_pitch && dst_pitch && len) {
                std::vector<u8> buf(len);
                for (u32 l = 0; l < lines; l++) {
                    const u64 s = as_.Translate(src + u64(l) * r[0x44]);
                    const u64 d = as_.Translate(dst + u64(l) * r[0x45]);
                    if (s && d && mem_.IsValid(s, len) && mem_.IsValid(d, len)) {
                        mem_.ReadBlock(s, buf.data(), len);
                        mem_.WriteBlock(d, buf.data(), len);
                    }
                }
            }
            const u32 sem = (arg >> 3) & 3;
            if (sem) {
                const u64 addr = (u64(r[0x90] & 0xFF) << 32) | r[0x91];
                write32(addr, r[0x92]);
                if (sem == 2)
                    write64(addr + 8, NowNs());
            }
        }
        return;
    }
}

} // namespace gpu
