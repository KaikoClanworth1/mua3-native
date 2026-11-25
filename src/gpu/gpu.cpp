#include "gpu/gpu.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include <windows.h>
#include <dbghelp.h>
#include <tlhelp32.h>
#pragma comment(lib, "dbghelp.lib")

#include "core/memory.h"
#include "gpu/renderer.h"

namespace gpu {

static Gpu* g_gpu = nullptr;
Gpu& G() { return *g_gpu; }

static std::atomic<u64> g_movie_plane[2][2]{}; // [luma, chroma][cpu, size]
static std::atomic<u64> g_movie_serial{0};
static std::atomic<s64> g_movie_until{0}; // steady_clock ticks
void NoteMoviePlanes(u64 luma_cpu, u64 luma_size, u64 chroma_cpu, u64 chroma_size) {
    g_movie_plane[0][0] = luma_cpu, g_movie_plane[0][1] = luma_size;
    g_movie_plane[1][0] = chroma_cpu, g_movie_plane[1][1] = chroma_size;
    g_movie_until.store((std::chrono::steady_clock::now() + std::chrono::seconds(10)).time_since_epoch().count(),
                        std::memory_order_relaxed);
    g_movie_serial.fetch_add(1, std::memory_order_release);
}
u64 MovieWatch() {
    const u64 serial = g_movie_serial.load(std::memory_order_acquire);
    const s64 until = g_movie_until.load(std::memory_order_relaxed);
    if (!serial || !until) // (the common case: one load, no clock read)
        return 0;
    if (std::chrono::steady_clock::now().time_since_epoch().count() > until) {
        g_movie_until.store(0, std::memory_order_relaxed);
        return 0;
    }
    return serial;
}
int MoviePlaneAt(u64 cpu) {
    if (!cpu)
        return 0;
    for (int p = 0; p < 2; p++) {
        const u64 base = g_movie_plane[p][0].load(std::memory_order_relaxed);
        if (base && cpu >= base && cpu < base + g_movie_plane[p][1].load(std::memory_order_relaxed))
            return p + 1;
    }
    return 0;
}
void Init(core::Memory& mem) { g_gpu = new Gpu(mem); }

// ---------------------------------------------------------------- AddressSpace
void AddressSpace::Map(u64 gpu_va, u64 cpu_va, u64 size) {
    std::lock_guard lk(mutex_);
    generation_++;
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
    // Small per-thread cache of recently hit mappings (the renderer translates thousands of addresses per frame).
    struct Hit {
        const AddressSpace* as;
        u64 gen, lo, hi, cpu;
    };
    thread_local Hit hits[8]{};
    thread_local u32 next_hit = 0;
    const u64 gen = generation_.load(std::memory_order_acquire);
    for (const Hit& h : hits)
        if (h.as == this && h.gen == gen && gpu_va >= h.lo && gpu_va < h.hi) {
            run = h.hi - gpu_va;
            return h.cpu + (gpu_va - h.lo);
        }
    std::lock_guard lk(mutex_);
    auto it = maps_.upper_bound(gpu_va);
    if (it == maps_.begin())
        return run = 0;
    --it;
    if (gpu_va >= it->first + it->second.size || !it->second.cpu_va)
        return run = 0;
    run = it->first + it->second.size - gpu_va;
    hits[next_hit++ & 7] = {this, gen, it->first, it->first + it->second.size, it->second.cpu_va};
    return it->second.cpu_va + (gpu_va - it->first);
}

u64 AddressSpace::Translate(u64 gpu_va) const {
    u64 run = 0;
    return TranslateRun(gpu_va, run);
}

// ---------------------------------------------------------------- Gpu
Gpu::Gpu(core::Memory& mem) : mem_(mem) {
    thread_ = std::thread([this] { ThreadMain(); });
    watchdog_ = std::thread([this] { WatchdogMain(); });
}

Gpu::~Gpu() {
    running_ = false;
    queue_cv_.notify_all();
    if (thread_.joinable())
        thread_.join();
    if (watchdog_.joinable())
        watchdog_.join();
}

// ---------------------------------------------------------------- watchdog stacks
// A thread's host stack: raw return addresses while it is suspended (it may hold the heap or loader lock, so nothing
// here allocates or names), named by StackText after it runs again.
static int WalkStack(HANDLE th, DWORD64 (&pcs)[24]) {
    int n = 0;
    if (SuspendThread(th) == DWORD(-1))
        return 0;
    CONTEXT ctx{};
    ctx.ContextFlags = CONTEXT_FULL;
    if (GetThreadContext(th, &ctx))
        for (; n < 24 && ctx.Rip; n++) {
            pcs[n] = ctx.Rip;
            DWORD64 image = 0;
            RUNTIME_FUNCTION* fn = RtlLookupFunctionEntry(ctx.Rip, &image, nullptr);
            if (!fn) { // leaf function: the return address is on top of the stack
                ctx.Rip = *reinterpret_cast<DWORD64*>(ctx.Rsp);
                ctx.Rsp += 8;
                continue;
            }
            void* handler_data = nullptr;
            DWORD64 frame = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, ctx.Rip, fn, &ctx, &handler_data, &frame, nullptr);
        }
    ResumeThread(th);
    return n;
}

static std::string StackText(const DWORD64* pcs, int n, const char* sep) {
    static const bool sym = [] {
        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
        char exe[MAX_PATH]{}; // the pdb sits next to the exe
        GetModuleFileNameA(nullptr, exe, MAX_PATH);
        std::string dir(exe);
        dir = dir.substr(0, dir.find_last_of("\\/"));
        SymInitialize(GetCurrentProcess(), dir.c_str(), TRUE); // (fails harmlessly if already done elsewhere)
        return true;
    }();
    (void)sym;
    std::string stack;
    for (int i = 0; i < n; i++) {
        alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 256]{};
        auto* si = reinterpret_cast<SYMBOL_INFO*>(buf);
        si->SizeOfStruct = sizeof(SYMBOL_INFO);
        si->MaxNameLen = 255;
        DWORD64 disp = 0;
        HMODULE mod = nullptr;
        char name[MAX_PATH] = "?";
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCSTR>(pcs[i]), &mod))
            GetModuleFileNameA(mod, name, MAX_PATH);
        const char* base = std::max(std::strrchr(name, '\\'), std::strrchr(name, '/'));
        base = base ? base + 1 : name;
        stack += sep;
        if (SymFromAddr(GetCurrentProcess(), pcs[i], &disp, si))
            stack += fmt::format("{}!{}+{:#x}", base, si->Name, disp);
        else
            stack += fmt::format("{}+{:#x}", base, mod ? pcs[i] - u64(mod) : pcs[i]);
    }
    return stack;
}

// Every other thread's stack (at a stall's first report: whoever holds the lock the GPU thread waits on shows up
// here). Each thread is suspended alone, walked and resumed before the next; the raw addresses are named afterwards
// by AllThreadText (naming loads the pdb the first time, which takes seconds; the walk itself is quick).
struct ThreadStack {
    DWORD id;
    std::string name;
    DWORD64 pcs[24];
    int n;
};
static std::vector<ThreadStack> WalkAllThreads(DWORD skip_a, DWORD skip_b) {
    std::vector<ThreadStack> ts;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return ts;
    THREADENTRY32 te{};
    te.dwSize = sizeof(te);
    const DWORD pid = GetCurrentProcessId();
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == skip_a || te.th32ThreadID == skip_b)
            continue;
        HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION, FALSE,
                               te.th32ThreadID);
        if (!th)
            continue;
        ThreadStack t{te.th32ThreadID, {}, {}, 0};
        PWSTR d = nullptr;
        if (SUCCEEDED(GetThreadDescription(th, &d)) && d) {
            for (const wchar_t* c = d; *c; c++)
                t.name += *c < 128 ? char(*c) : '?';
            LocalFree(d);
        }
        t.n = WalkStack(th, t.pcs);
        CloseHandle(th);
        ts.push_back(std::move(t));
    }
    CloseHandle(snap);
    return ts;
}
static std::string AllThreadText(const std::vector<ThreadStack>& ts) {
    if (ts.empty())
        return "\n    (no thread snapshot)";
    std::string out;
    for (const ThreadStack& t : ts)
        out += fmt::format("\n  thread {} '{}':{}", t.id, t.name, StackText(t.pcs, t.n, " < "));
    return out;
}

void Gpu::WatchdogMain() {
    SetThreadDescription(GetCurrentThread(), L"GPU watchdog");
    // A stall: no job finished for 5 s while one runs or some wait in the queue (a thread stuck in a job, or one that
    // stopped taking them). Idle with an empty queue is not a stall.
    u64 last_done = ~0ull;
    u64 since = NowNs(); // when a job last finished, or the thread was last idle with nothing queued
    double next_report = 0, last_cpu = 0, last_s = 0;
    bool all_dumped = false; // this stall's every-thread dump was written
    // the thread's CPU time beside its stack: climbing with a stack that does not move is a spin, flat is a wait
    auto cpu_s = [&] {
        FILETIME c, e, k, u;
        if (!GetThreadTimes(static_cast<HANDLE>(thread_.native_handle()), &c, &e, &k, &u))
            return 0.0;
        auto v = [](const FILETIME& f) { return double((u64(f.dwHighDateTime) << 32) | f.dwLowDateTime) / 1e7; };
        return v(k) + v(u);
    };
    while (running_) {
        Sleep(500);
        const u64 start = job_start_ns_.load(std::memory_order_relaxed);
        const u64 done = jobs_done_.load(std::memory_order_relaxed);
        size_t queued;
        {
            std::lock_guard lk(queue_mutex_);
            queued = queue_.size();
        }
        if (done != last_done || (!start && !queued)) {
            last_done = done, since = NowNs(), next_report = 0, all_dumped = false;
            last_cpu = cpu_s(), last_s = 0;
            continue;
        }
        const double s = double(NowNs() - since) / 1e9;
        if (s < 5) { // (the baseline for the first report: the last poll before it)
            last_cpu = cpu_s(), last_s = s;
            continue;
        }
        if (s < next_report)
            continue;
        next_report = s < 10 ? 10 : s * 2; // at 5 s, 10 s, 20 s, 40 s ...
        HANDLE th = static_cast<HANDLE>(thread_.native_handle());
        DWORD64 pcs[24];
        const int n = WalkStack(th, pcs);
        // At the first report of a stall, every other thread too (once per stall): the GPU thread is usually blocked
        // on a lock (the D3D11 context mutex) and its holder is somewhere else. Walked now, named below.
        const bool all = !all_dumped;
        all_dumped = true;
        std::vector<ThreadStack> others;
        if (all)
            others = WalkAllThreads(GetThreadId(th), GetCurrentThreadId());
        // its CPU use up to now, before naming the stack (the first naming loads the pdb, seconds)
        const double cpu = cpu_s(), now_s = double(NowNs() - since) / 1e9;
        const double used = now_s > last_s ? (cpu - last_cpu) / (now_s - last_s) * 100 : 0;
        last_cpu = cpu, last_s = now_s;
        const std::string stack = StackText(pcs, n, "\n    ");
        const int kind = job_kind_.load(std::memory_order_relaxed);
        const std::string what = start ? fmt::format("in one {} job", kind == Job::Present ? "present"
                                                                      : kind == Job::Host  ? "command"
                                                                                           : "?")
                                       : std::string("between jobs, not taking the next one");
        LOG_WARN("GPU", "GPU thread waiting {:.0f} s {} ({} queued, {} jobs done), {:.0f} % of a CPU since the last check ({}); "
                        "host stack:{}",
                 s, what, queued, done, used, used > 50 ? "spinning" : "waiting", stack);
        if (all)
            LOG_WARN("GPU", "GPU thread waiting {:.0f} s: every other thread's host stack (innermost first):{}", s,
                     AllThreadText(others));
    }
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
    if (gpu_va >= kHostBase) {
        if (gpu_va + size > kHostBase + kHostSize)
            return std::memset(d, 0, size), false;
        std::memcpy(d, &host_mem_[gpu_va - kHostBase], size);
        return true;
    }
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
    if (gpu_va >= kHostBase)
        return gpu_va + size <= kHostBase + kHostSize ? &host_mem_[gpu_va - kHostBase] : nullptr;
    u64 run = 0;
    const u64 cpu = as_.TranslateRun(gpu_va, run);
    if (!cpu || run < size || !mem_.IsValid(cpu, size))
        return nullptr;
    return mem_.Ptr(cpu);
}

size_t Gpu::ReadGpuPartial(u64 gpu_va, void* dst, size_t size) {
    u8* d = static_cast<u8*>(dst);
    if (gpu_va >= kHostBase) {
        const size_t n = gpu_va < kHostBase + kHostSize ? std::min<size_t>(size, kHostBase + kHostSize - gpu_va) : 0;
        std::memcpy(d, &host_mem_[gpu_va - kHostBase], n);
        return n;
    }
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
    BumpMemEpoch();
    const u8* s = static_cast<const u8*>(src);
    if (gpu_va >= kHostBase) {
        if (gpu_va + size > kHostBase + kHostSize)
            return false;
        std::memcpy(&host_mem_[gpu_va - kHostBase], s, size);
        return true;
    }
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
    if (!done() && timeout_us < 0)
        LOG_INFO("GPU", "waiting for syncpoint {} >= {} (now {}, max {}, timeout {}us)", id, value, syncpoints_[id].load(),
                  syncpoint_max_[id].load(), timeout_us);
    if (timeout_us < 0) {
        sync_cv_.wait(lk, done);
        return true;
    }
    return sync_cv_.wait_for(lk, std::chrono::microseconds(timeout_us), done);
}

void Gpu::CompleteFence(u32 syncpt_id, u32 target) {
    BumpMemEpoch();
    submits++;
    SubmitHost([this, syncpt_id, target] {
        if (syncpt_id >= kNumSyncpoints)
            return;
        const s32 behind = s32(target - syncpoints_[syncpt_id].load());
        if (behind > 0)
            IncrementSyncpoint(syncpt_id, u32(behind));
    });
}

void Gpu::SubmitHost(std::function<void()> fn) {
    {
        std::lock_guard lk(queue_mutex_);
        Job j{Job::Host};
        j.host = std::move(fn);
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
        job_kind_.store(int(job.kind), std::memory_order_relaxed);
        job_start_ns_.store(NowNs(), std::memory_order_relaxed);
        struct JobDone {
            Gpu* g;
            ~JobDone() {
                g->job_start_ns_.store(0, std::memory_order_relaxed);
                g->jobs_done_.fetch_add(1, std::memory_order_relaxed);
            }
        } job_done{this};
        switch (job.kind) {
        case Job::Host: {
            const u64 t0 = NowNs();
            job.host();
            busy_ns += NowNs() - t0;
            if (renderer_)
                renderer_->FlushCommands();
            break;
        }
        case Job::Present:
            if (renderer_) {
                renderer_->Present(job.present);
                renderer_->ThrottleFrames();
            }
            presents++;
            if (on_present)
                on_present(job.present);
            break;
        default:
            break;
        }
    }
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
    if (const u64 movie = MovieWatch(); movie && !dst_pitch) { // (#22) per movie, its first 4 copies into textures
        static u64 movie_seen = 0;
        static int count = 0;
        if (movie_seen != movie)
            movie_seen = movie, count = 0;
        if (count < 4) {
            count++;
            const u64 src_cpu = As().Translate(src);
            LOG_INFO("GPU", "movie DMA {}: src {:#x} ({}, cpu {:#x}, movie plane {}) -> dst {:#x} block-linear, {} bytes x {} "
                            "lines, pitch in {}, remap {} ({:#x}), dst surface {:#x} {:#x} {:#x}",
                     count, src, src_pitch ? "pitch" : "block-linear", src_cpu, MoviePlaneAt(src_cpu), dst, len, lines,
                     pitch_in, remap, r[0x1C2], r[0x1C3], r[0x1C4], r[0x1C5]);
        }
    }

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
        BumpMemEpoch();
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


} // namespace gpu
