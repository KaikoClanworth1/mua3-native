#pragma once
#include <atomic>
#include <map>
#include <mutex>
#include "common/common.h"

namespace core {

// Horizon MemoryState values (6.x).
enum MemState : u32 {
    MS_Free = 0x00000000,
    MS_Io = 0x00182001,
    MS_Static = 0x00042002,
    MS_Code = 0x00DC7E03,
    MS_CodeData = 0x03FEBD04,
    MS_Normal = 0x037EBD05,
    MS_Shared = 0x00402006,
    MS_Alias = 0x00482907,
    MS_AliasCode = 0x00DD7E08,
    MS_AliasCodeData = 0x03FFBD09,
    MS_Ipc = 0x005C3C0A,
    MS_Stack = 0x005C3C0B,
    MS_ThreadLocal = 0x0040200C,
    MS_Transfered = 0x015C3C0D,
    MS_SharedTransfered = 0x005C380E,
    MS_SharedCode = 0x0040380F,
    MS_Inaccessible = 0x00000010,
};

enum MemPerm : u32 { MP_None = 0, MP_R = 1, MP_W = 2, MP_X = 4, MP_RW = 3, MP_RX = 5, MP_DontCare = 0x10000000 };

struct MemoryInfo {
    u64 addr;
    u64 size;
    u32 state;
    u32 attr;
    u32 perm;
    u32 ipc_refcount;
    u32 device_refcount;
    u32 pad;
};
static_assert(sizeof(MemoryInfo) == 0x28);

// 39-bit guest address space layout (application, no ASLR).
namespace layout {
constexpr u64 kSpaceBits = 39;
constexpr u64 kSpaceEnd = 1ull << kSpaceBits;
constexpr u64 kCodeBase = 0x0008000000ull;
constexpr u64 kAslrBase = 0x0008000000ull;
constexpr u64 kAslrSize = kSpaceEnd - kAslrBase;
constexpr u64 kAliasBase = 0x1000000000ull;
constexpr u64 kAliasSize = 0x1000000000ull;
constexpr u64 kHeapBase = 0x2000000000ull;
constexpr u64 kHeapSize = 0x0180000000ull;
constexpr u64 kStackBase = 0x3000000000ull;
constexpr u64 kStackSize = 0x0080000000ull;
constexpr u64 kKernelMapBase = 0x4000000000ull; // where the "kernel" picks addresses (TLS, shared memory)
constexpr u64 kTotalMemory = 0xCD500000ull;     // application pool
} // namespace layout

class Memory {
public:
    Memory();
    ~Memory();

    u8* Base() const { return base_; }
    template <typename T = u8>
    T* Ptr(u64 addr) const {
        return reinterpret_cast<T*>(base_ + addr);
    }
    bool IsValid(u64 addr, u64 size = 1) const;

    template <typename T>
    T Read(u64 addr) const {
        T v;
        std::memcpy(&v, base_ + addr, sizeof(T));
        return v;
    }
    template <typename T>
    void Write(u64 addr, const T& v) {
        std::memcpy(base_ + addr, &v, sizeof(T));
    }
    void ReadBlock(u64 addr, void* dst, size_t size) const { std::memcpy(dst, base_ + addr, size); }
    void WriteBlock(u64 addr, const void* src, size_t size) { std::memcpy(base_ + addr, src, size); }
    std::string ReadString(u64 addr, size_t max = 0x1000) const;

    // Commits host memory and records the region.
    bool Map(u64 addr, u64 size, u32 state, u32 perm);
    bool Unmap(u64 addr, u64 size);
    void SetState(u64 addr, u64 size, u32 state, u32 perm);
    void SetPerm(u64 addr, u64 size, u32 perm);
    void SetAttr(u64 addr, u64 size, u32 mask, u32 value);
    MemoryInfo Query(u64 addr);
    // Finds a free range inside [region, region+region_size).
    u64 FindFree(u64 region, u64 region_size, u64 size, u64 align = 0x1000);

    // svcSetHeapSize.
    bool SetHeapSize(u64 size);
    u64 HeapSize() const { return heap_size_; }
    u64 UsedMemory() const { return used_; }

    std::mutex& Lock() { return mutex_; }

private:
    struct Vma {
        u64 size;
        u32 state;
        u32 perm;
        u32 attr;
    };
    void SplitAt(u64 addr);
    void Insert(u64 addr, u64 size, u32 state, u32 perm, u32 attr);

    void MarkPages(u64 addr, u64 size, bool committed);

    u8* base_ = nullptr;
    std::vector<std::atomic<u64>> page_bits_; // 1 bit per committed 4 KiB page
    std::map<u64, Vma> vmas_; // non-free regions only
    u64 heap_size_ = 0;
    u64 used_ = 0;
    std::recursive_mutex vma_mutex_;
    std::mutex mutex_;
};

} // namespace core
