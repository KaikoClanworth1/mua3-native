#pragma once
#include <atomic>
#include <functional>
#include <map>
#include <mutex>

#include "common/common.h"

namespace core {
class Memory;
}

namespace gpu {

constexpr u32 kNumSyncpoints = 192;

// GPU virtual address space (one per nvhost-as-gpu fd; MUA3 uses a single one).
class AddressSpace {
public:
    void Map(u64 gpu_va, u64 cpu_va, u64 size);
    void Unmap(u64 gpu_va, u64 size);
    u64 Allocate(u64 size, u64 align);
    void Reserve(u64 gpu_va, u64 size);
    // Returns the CPU address for `gpu_va`, or 0 if unmapped.
    u64 Translate(u64 gpu_va) const;

private:
    struct Mapping {
        u64 size;
        u64 cpu_va;
    };
    mutable std::mutex mutex_;
    std::map<u64, Mapping> maps_;
    u64 next_ = 0x0400000000ull;
};

class Gpu {
public:
    explicit Gpu(core::Memory& mem);

    AddressSpace& As() { return as_; }

    u32 SyncpointValue(u32 id) const { return id < kNumSyncpoints ? syncpoints_[id].load() : 0; }
    u32 IncrementSyncpoint(u32 id, u32 count = 1);
    void SetSyncpoint(u32 id, u32 v);

    // Executes a GPFIFO submission. Today only the parts the CPU can observe are handled
    // (semaphore releases, reports, inline memory writes); rendering is not performed yet.
    void Submit(const u64* entries, u32 count);

    // Called once per guest present (buffer queue). Hook point for the D3D11 backend.
    std::function<void(u64 cpu_addr, u32 width, u32 height, u32 stride, u32 format)> on_present;

    u64 submits = 0, methods = 0;

private:
    void ProcessPushbuffer(u64 gpu_va, u32 words);
    void CallMethod(u32 subch, u32 method, u32 arg);
    u64 NowNs() const;

    core::Memory& mem_;
    AddressSpace as_;
    std::atomic<u32> syncpoints_[kNumSyncpoints]{};
    std::mutex submit_mutex_;
    u32 bound_class_[8]{};
    u32 regs_[8][0x1000]{};
};

Gpu& G();
void Init(core::Memory& mem);

} // namespace gpu
