#include <mutex>
#include <set>

#include <dynarmic/interface/A64/a64.h>
#include <dynarmic/interface/A64/config.h>
#include <dynarmic/interface/exclusive_monitor.h>

#include "core/cpu/cpu.h"
#include "core/memory.h"

namespace cpu {

namespace {

constexpr auto kHaltStop = Dynarmic::HaltReason::UserDefined1;
constexpr auto kHaltInvalidate = Dynarmic::HaltReason::UserDefined2;
constexpr auto kHaltCallback = Dynarmic::HaltReason::UserDefined3;

class DynarmicFactory;

class DynarmicCore final : public Core, public Dynarmic::A64::UserCallbacks {
public:
    DynarmicCore(DynarmicFactory& f, core::Memory& mem, Dynarmic::ExclusiveMonitor& monitor, CpuCallbacks cb,
                 size_t slot);
    ~DynarmicCore() override;

    // ---- Core
    void Run() override {
        stop_ = false;
        while (!stop_) {
            const auto hr = jit_->Run();
            if (Dynarmic::Has(hr, kHaltInvalidate)) {
                jit_->ClearHalt(kHaltInvalidate);
                ApplyInvalidations();
            }
            if (Dynarmic::Has(hr, kHaltCallback)) {
                jit_->ClearHalt(kHaltCallback);
                std::vector<std::function<void()>> fns;
                {
                    std::lock_guard lk(inv_mutex_);
                    fns.swap(callbacks_);
                }
                for (auto& f : fns)
                    f();
            }
            if (Dynarmic::Has(hr, kHaltStop)) {
                jit_->ClearHalt(kHaltStop);
                break;
            }
        }
    }
    void Halt() override {
        stop_ = true;
        jit_->HaltExecution(kHaltStop);
    }
    void Step() override { jit_->Step(); }
    u64 Reg(int i) const override { return jit_->GetRegister(i); }
    void SetReg(int i, u64 v) override { jit_->SetRegister(i, v); }
    u64 PC() const override { return jit_->GetPC(); }
    void SetPC(u64 v) override { jit_->SetPC(v); }
    u64 SP() const override { return jit_->GetSP(); }
    void SetSP(u64 v) override { jit_->SetSP(v); }
    Vec128 Vector(int i) const override { return jit_->GetVector(i); }
    void SetVector(int i, Vec128 v) override { jit_->SetVector(i, v); }
    u32 Fpcr() const override { return jit_->GetFpcr(); }
    void SetFpcr(u32 v) override { jit_->SetFpcr(v); }
    u32 Fpsr() const override { return jit_->GetFpsr(); }
    void SetFpsr(u32 v) override { jit_->SetFpsr(v); }
    u32 Pstate() const override { return jit_->GetPstate(); }
    void SetPstate(u32 v) override { jit_->SetPstate(v); }
    u64 TpidrEl0() const override { return tpidr_el0_; }
    void SetTpidrEl0(u64 v) override { tpidr_el0_ = v; }
    void SetTpidrroEl0(u64 v) override { tpidrro_el0_ = v; }
    void InvalidateCache(u64 addr, u64 size) override { jit_->InvalidateCacheRange(addr, size); }
    void ClearExclusive() override { jit_->ClearExclusiveState(); }

    void RunOnCore(std::function<void()> fn) override {
        {
            std::lock_guard lk(inv_mutex_);
            callbacks_.push_back(std::move(fn));
        }
        jit_->HaltExecution(kHaltCallback);
    }
    void QueueInvalidate(u64 addr, u64 size) {
        {
            std::lock_guard lk(inv_mutex_);
            pending_.emplace_back(addr, size);
        }
        jit_->HaltExecution(kHaltInvalidate);
    }
    void ApplyInvalidations() {
        std::lock_guard lk(inv_mutex_);
        for (auto [a, s] : pending_)
            jit_->InvalidateCacheRange(a, s);
        pending_.clear();
    }

    // ---- UserCallbacks
    std::optional<u32> MemoryReadCode(u64 vaddr) override {
        if (!mem_.IsValid(vaddr, 4))
            return std::nullopt;
        return mem_.Read<u32>(vaddr);
    }
    template <typename T>
    T SlowRead(u64 a) {
        if (!mem_.IsValid(a, sizeof(T))) {
            LOG_ERROR("CPU", "Invalid read{} at {:#x} (pc {:#x})", sizeof(T) * 8, a, jit_->GetPC());
            return T{};
        }
        return mem_.Read<T>(a);
    }
    template <typename T>
    void SlowWrite(u64 a, T v) {
        if (!mem_.IsValid(a, sizeof(T))) {
            LOG_ERROR("CPU", "Invalid write{} at {:#x} (pc {:#x})", sizeof(T) * 8, a, jit_->GetPC());
            return;
        }
        mem_.Write<T>(a, v);
    }
    template <typename T>
    bool SlowWriteEx(u64 a, T v, T expected) {
        if (!mem_.IsValid(a, sizeof(T)))
            return false;
        auto* p = mem_.Ptr<T>(a);
        return std::atomic_ref<T>(*p).compare_exchange_strong(expected, v);
    }
    u8 MemoryRead8(u64 a) override { return SlowRead<u8>(a); }
    u16 MemoryRead16(u64 a) override { return SlowRead<u16>(a); }
    u32 MemoryRead32(u64 a) override { return SlowRead<u32>(a); }
    u64 MemoryRead64(u64 a) override { return SlowRead<u64>(a); }
    Dynarmic::A64::Vector MemoryRead128(u64 a) override { return {SlowRead<u64>(a), SlowRead<u64>(a + 8)}; }
    void MemoryWrite8(u64 a, u8 v) override { SlowWrite(a, v); }
    void MemoryWrite16(u64 a, u16 v) override { SlowWrite(a, v); }
    void MemoryWrite32(u64 a, u32 v) override { SlowWrite(a, v); }
    void MemoryWrite64(u64 a, u64 v) override { SlowWrite(a, v); }
    void MemoryWrite128(u64 a, Dynarmic::A64::Vector v) override {
        SlowWrite(a, v[0]);
        SlowWrite(a + 8, v[1]);
    }
    bool MemoryWriteExclusive8(u64 a, u8 v, u8 e) override { return SlowWriteEx(a, v, e); }
    bool MemoryWriteExclusive16(u64 a, u16 v, u16 e) override { return SlowWriteEx(a, v, e); }
    bool MemoryWriteExclusive32(u64 a, u32 v, u32 e) override { return SlowWriteEx(a, v, e); }
    bool MemoryWriteExclusive64(u64 a, u64 v, u64 e) override { return SlowWriteEx(a, v, e); }
    bool MemoryWriteExclusive128(u64 a, Dynarmic::A64::Vector v, Dynarmic::A64::Vector e) override {
        if (!mem_.IsValid(a, 16))
            return false;
        auto* p = mem_.Ptr<long long>(a);
        long long cmp[2] = {(long long)e[0], (long long)e[1]};
        return _InterlockedCompareExchange128(p, (long long)v[1], (long long)v[0], cmp) != 0;
    }

    void InterpreterFallback(u64 pc, size_t n) override {
        PANIC("Interpreter fallback at {:#x} ({} instructions)", pc, n);
    }
    void CallSVC(u32 swi) override { cb_.svc(swi); }
    void ExceptionRaised(u64 pc, Dynarmic::A64::Exception e) override {
        using E = Dynarmic::A64::Exception;
        switch (e) {
        case E::Yield:
        case E::WaitForEvent:
        case E::WaitForInterrupt:
        case E::SendEvent:
        case E::SendEventLocal:
            return;
        default:
            jit_->SetPC(pc);
            cb_.exception(pc, int(e));
        }
    }
    void AddTicks(u64) override {}
    u64 GetTicksRemaining() override { return 200000; }
    u64 GetCNTPCT() override { return cb_.cntpct(); }

    size_t slot_;

private:
    DynarmicFactory& factory_;
    core::Memory& mem_;
    CpuCallbacks cb_;
    u64 tpidr_el0_ = 0, tpidrro_el0_ = 0;
    std::unique_ptr<Dynarmic::A64::Jit> jit_;
    volatile bool stop_ = false;
    std::mutex inv_mutex_;
    std::vector<std::pair<u64, u64>> pending_;
    std::vector<std::function<void()>> callbacks_;
};

class DynarmicFactory final : public CoreFactory {
public:
    DynarmicFactory(core::Memory& mem, size_t max_cores) : mem_(mem), monitor_(max_cores), max_(max_cores) {}

    std::unique_ptr<Core> Create(CpuCallbacks cb, size_t) override {
        size_t slot;
        {
            std::lock_guard lk(mutex_);
            slot = 0;
            while (used_slots_.count(slot))
                slot++;
            if (slot >= max_)
                PANIC("Too many guest threads ({})", slot);
            used_slots_.insert(slot);
        }
        auto core = std::make_unique<DynarmicCore>(*this, mem_, monitor_, std::move(cb), slot);
        std::lock_guard lk(mutex_);
        cores_.insert(core.get());
        return core;
    }
    void Release(DynarmicCore* c) {
        std::lock_guard lk(mutex_);
        cores_.erase(c);
        used_slots_.erase(c->slot_);
    }
    void InvalidateAll(u64 addr, u64 size) override {
        std::lock_guard lk(mutex_);
        for (auto* c : cores_)
            c->QueueInvalidate(addr, size);
    }

private:
    core::Memory& mem_;
    Dynarmic::ExclusiveMonitor monitor_;
    size_t max_;
    std::mutex mutex_;
    std::set<DynarmicCore*> cores_;
    std::set<size_t> used_slots_;
};

DynarmicCore::DynarmicCore(DynarmicFactory& f, core::Memory& mem, Dynarmic::ExclusiveMonitor& monitor,
                           CpuCallbacks cb, size_t slot)
    : slot_(slot), factory_(f), mem_(mem), cb_(std::move(cb)) {
    Dynarmic::A64::UserConfig c{};
    c.callbacks = this;
    c.processor_id = slot;
    c.global_monitor = &monitor;
    c.tpidrro_el0 = &tpidrro_el0_;
    c.tpidr_el0 = &tpidr_el0_;
    c.cntfrq_el0 = 19200000;
    c.dczid_el0 = 4;
    c.ctr_el0 = 0x8444c004;
    c.fastmem_pointer = reinterpret_cast<uintptr_t>(mem.Base());
    c.fastmem_address_space_bits = core::layout::kSpaceBits;
    c.silently_mirror_fastmem = false;
    c.fastmem_exclusive_access = true;
    c.recompile_on_fastmem_failure = true;
    c.wall_clock_cntpct = true;
    c.enable_cycle_counting = true; // return to the dispatcher regularly so halts are always seen
    c.define_unpredictable_behaviour = true;
    c.code_cache_size = 128ull << 20;
    jit_ = std::make_unique<Dynarmic::A64::Jit>(c);
}

DynarmicCore::~DynarmicCore() { factory_.Release(this); }

} // namespace

std::unique_ptr<CoreFactory> CreateDynarmicFactory(core::Memory& memory, size_t max_cores) {
    return std::make_unique<DynarmicFactory>(memory, max_cores);
}

} // namespace cpu
