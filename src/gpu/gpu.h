#pragma once
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
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
    Maxwell3D& M3D() { return m3d_; }

    void SetRenderer(Renderer* r);
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
        std::vector<u64> entries;
        u32 syncpt_id = 0, syncpt_incr = 0;
        PresentInfo present;
    };
    void ThreadMain();
    void RunCommands(const std::vector<u64>& entries);
    void ProcessPushbuffer(u64 gpu_va, u32 words);
    void CallMethod(u32 subch, u32 method, u32 arg, bool last_in_batch);
    void FlushMacro();
    void RunDma(const u32* regs, u32 launch);

    core::Memory& mem_;
    AddressSpace as_;
    Maxwell3D m3d_;
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
