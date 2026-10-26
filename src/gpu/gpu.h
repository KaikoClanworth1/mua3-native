#pragma once
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

#include "common/common.h"
#include "gpu/maxwell3d.h"

namespace core {
class Memory;
}

namespace gpu {

constexpr u32 kNumSyncpoints = 192;

class Renderer;

// GPU virtual address space (one per nvhost-as-gpu fd; MUA3 uses a single one).
class AddressSpace {
public:
    void Map(u64 gpu_va, u64 cpu_va, u64 size);
    void Unmap(u64 gpu_va, u64 size);
    u64 Allocate(u64 size, u64 align);
    void Reserve(u64 gpu_va, u64 size);
    // Returns the CPU address for `gpu_va`, or 0 if unmapped.
    u64 Translate(u64 gpu_va) const;
    // Like Translate, also returning how many bytes stay contiguous in the same mapping.
    u64 TranslateRun(u64 gpu_va, u64& run) const;

private:
    struct Mapping {
        u64 size;
        u64 cpu_va;
    };
    mutable std::mutex mutex_;
    std::map<u64, Mapping> maps_;
    std::atomic<u64> generation_{1}; // bumped on every Map/Unmap: invalidates the per-thread lookup caches
    u64 next_ = 0x0400000000ull;
};

struct PresentInfo {
    u64 cpu_addr = 0;
    u32 width = 0, height = 0, stride = 0, format = 0;
};

class Gpu {
public:
    explicit Gpu(core::Memory& mem);
    ~Gpu();

    core::Memory& Mem() { return mem_; }
    AddressSpace& As() { return as_; }
    // The 3D engine the renderer reads: the command stream's, or the native NVN replay's (gpu/nvn_encoder.cpp).
    Maxwell3D& M3D() { return *active_m3d_; }
    Maxwell3D& StreamM3D() { return m3d_; }
    void SetActiveM3D(Maxwell3D* m) { active_m3d_ = m ? m : &m3d_; }

    // Host-backed GPU memory at [kHostBase, kHostBase + kHostSize): constant buffers the native replay builds itself
    // (driver constants, texture handles). Reads and writes through the helpers below reach it like mapped memory.
    static constexpr u64 kHostBase = 0xFF00000000ull, kHostSize = 0x4000000;
    u8* HostMem() { return host_mem_.get(); }

    // Verification (MUA3_NVN_VERIFY): the next Submit() after tag_next_submit is set runs with on_tagged_job called
    // first, on the GPU thread.
    std::atomic<bool> tag_next_submit{false};
    std::function<void()> on_tagged_job;
    // Sees every compute launch before the renderer; returns true if it took care of it (native replay).
    std::function<bool(const u32* regs, const u32* qmd)> intercept_dispatch;

    void SetRenderer(Renderer* r);
    // Native handle of the GPU command thread (profiling).
    void* NativeThread() { return thread_.native_handle(); }
    Renderer* GetRenderer() const { return renderer_; }

    // ---- syncpoints
    u32 SyncpointValue(u32 id) const { return id < kNumSyncpoints ? syncpoints_[id].load() : 0; }
    u32 SyncpointMax(u32 id) const { return id < kNumSyncpoints ? syncpoint_max_[id].load() : 0; }
    // Reserves `count` increments on `id` (done when the GPU thread reaches them) and returns the target value.
    u32 ReserveSyncpoint(u32 id, u32 count);
    void IncrementSyncpoint(u32 id, u32 count = 1); // immediate (host-side) increment
    // Waits until syncpoint `id` reaches `value`. timeout_us < 0 waits forever. Returns false on timeout.
    bool WaitSyncpoint(u32 id, u32 value, s64 timeout_us);

    // ---- submission (any thread). Work runs in order on the GPU thread.
    void Submit(std::vector<u64> entries, u32 syncpt_id, u32 syncpt_incr);
    void QueuePresent(const PresentInfo& p);
    void WaitIdle();
    void* NativeThreadHandle() { return thread_.native_handle(); }

    // Called on the GPU thread after a frame has been presented (for stats / UI).
    std::function<void(const PresentInfo&)> on_present;

    // ---- helpers used by engines (GPU thread)
    bool ReadGpu(u64 gpu_va, void* dst, size_t size);
    // Reads up to `size` bytes, stopping at the first unmapped page; returns the number of bytes read.
    size_t ReadGpuPartial(u64 gpu_va, void* dst, size_t size);
    bool WriteGpu(u64 gpu_va, const void* src, size_t size);
    // Host pointer for a GPU range if it is mapped contiguously to valid guest memory, else null.
    u8* GpuPtr(u64 gpu_va, size_t size);
    u64 NowNs() const;

    std::atomic<u64> submits{0}, methods{0}, draws{0}, presents{0};
    std::atomic<u64> busy_ns{0}, draw_ns{0}; // time the GPU thread spent executing commands / in Renderer::Draw

private:
    struct Job {
        enum Kind { Commands, Present, Fence } kind;
        bool tagged = false;
        std::vector<u64> entries;
        u32 syncpt_id = 0, syncpt_incr = 0;
        PresentInfo present;
    };
    void ThreadMain();
    void RunCommands(const std::vector<u64>& entries);
    void ProcessPushbuffer(u64 gpu_va, u32 words);
    u32 dma_mode_ = 0, dma_subch_ = 0, dma_method_ = 0, dma_count_ = 0, dma_k_ = 0; // pending method data
    void CallMethod(u32 subch, u32 method, u32 arg, bool last_in_batch);
    void FlushMacro();
    void RunDma(const u32* regs, u32 launch);

    core::Memory& mem_;
    AddressSpace as_;
    Maxwell3D m3d_;
    Maxwell3D* active_m3d_ = &m3d_;
    std::unique_ptr<u8[]> host_mem_{new u8[kHostSize]{}};
    Renderer* renderer_ = nullptr;

    std::atomic<u32> syncpoints_[kNumSyncpoints]{};
    std::atomic<u32> syncpoint_max_[kNumSyncpoints]{};
    std::mutex sync_mutex_;
    std::condition_variable sync_cv_;

    std::mutex queue_mutex_;
    std::condition_variable queue_cv_, idle_cv_;
    std::deque<Job> queue_;
    bool busy_ = false;
    std::atomic<bool> running_{true};
    std::thread thread_;

    // Per-subchannel engine binding and generic register files for the simple engines.
    u32 bound_class_[8]{};
    std::vector<u32> regs_[8];
    // Pending macro call (params accumulate until the batch ends).
    s32 macro_index_ = -1;
    std::vector<u32> macro_params_;
};

Gpu& G();
void Init(core::Memory& mem);

} // namespace gpu
