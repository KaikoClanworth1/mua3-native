// Inline caches for the generated code's register calls and jumps (see the block at the end of recomp_rt.h).
// A site that hits its cache calls the target directly; a miss goes through Dispatch as before and fills the slot.
#include <atomic>

#include "common/common.h"
#include "core/cpu/recomp/recomp_rt.h"

namespace recomp {

// cpu_dynarmic.cpp: the function Dispatch would run for s.pc, or nullptr when the call must take Dispatch's own
// path (no native code, Dispatch census, NVN name capture)
Fn IcResolve(State& s);

// Bumped before every clear; entries carry its low 4 bits, so one stored from a target resolved before a change
// never matches after it. (After 16 changes the tag would repeat: a miss that resolved its target before a clear
// and stores it after re-reads the epoch and takes the entry back.)
std::atomic<u32> g_ic_epoch{0};
namespace {
constexpr u64 kPcLimit = 1ull << 28, kFnLimit = 1ull << 32;
} // namespace

void DispatchIc(State& s, u64& slot) {
    const u32 epoch = g_ic_epoch.load(std::memory_order_acquire);
    const Fn fn = IcResolve(s);
    if (!fn) {
        Dispatch(s);
        return;
    }
    const u64 off = u64(reinterpret_cast<uintptr_t>(fn)) - u64(reinterpret_cast<uintptr_t>(&__ImageBase));
    if (s.pc < kPcLimit && off < kFnLimit) {
        __iso_volatile_store64(reinterpret_cast<__int64*>(&slot), __int64((((s.pc ^ 0xFFFFFFFull) << 4 | (epoch & 15)) << 32) | off));
        std::atomic_thread_fence(std::memory_order_seq_cst); // the store before the epoch re-read (StoreLoad)
        if (g_ic_epoch.load(std::memory_order_relaxed) != epoch)
            __iso_volatile_store64(reinterpret_cast<__int64*>(&slot), 0);
    }
    fn(s);
}

// Every change of the native table (functions added, replaced by host code, disabled).
void IcClear() {
    g_ic_epoch.fetch_add(1, std::memory_order_seq_cst);
    for (u32 i = 0; i < kIcShardCount; i++)
        for (u32 k = 0; k < kIcShards[i].count; k++)
            __iso_volatile_store64(reinterpret_cast<__int64*>(&kIcShards[i].slots[k]), 0);
}

// Once, when the native table is built: the library's shards and its table must come from one generation (a stale
// shard object would index past its slots).
void IcCheck() {
    u64 sites = 0;
    for (u32 i = 0; i < kIcShardCount; i++) {
        if (*kIcShards[i].emitted != kIcShards[i].count)
            PANIC("recompiled library is inconsistent: shard {} has {} inline-cache sites, its table says {} "
                  "(rebuild nxrecomp_full.lib with scripts/recomp_libbuild.py)",
                  i, *kIcShards[i].emitted, kIcShards[i].count);
        sites += kIcShards[i].count;
    }
    LOG_INFO("Recomp", "{} inline-cached call sites in {} shards", sites, kIcShardCount);
}

} // namespace recomp
