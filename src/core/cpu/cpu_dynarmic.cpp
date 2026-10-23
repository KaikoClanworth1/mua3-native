#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <set>
#include <shared_mutex>
#include <thread>
#include <unordered_map>

#include <windows.h>

#include <dynarmic/interface/A64/a64.h>
#include <dynarmic/interface/A64/config.h>
#include <dynarmic/interface/exclusive_monitor.h>

#include "core/cpu/cpu.h"
#include "core/cpu/recomp/recomp_rt.h"
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
            native_halt_ = 0;
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
        native_halt_ = 1;
        jit_->HaltExecution(kHaltStop);
    }
    void Step() override { jit_->Step(); }
    // While recompiled code runs (native_), the register file lives in st_.
    u64 Reg(int i) const override { return native_ ? st_.x[i] : jit_->GetRegister(i); }
    void SetReg(int i, u64 v) override { native_ ? void(st_.x[i] = v) : jit_->SetRegister(i, v); }
    u64 PC() const override { return native_ ? st_.pc : jit_->GetPC(); }
    void SetPC(u64 v) override { native_ ? void(st_.pc = v) : jit_->SetPC(v); }
    u64 SP() const override { return native_ ? st_.sp : jit_->GetSP(); }
    void SetSP(u64 v) override { native_ ? void(st_.sp = v) : jit_->SetSP(v); }
    Vec128 Vector(int i) const override {
        return native_ ? Vec128{st_.v[i].lo, st_.v[i].hi} : jit_->GetVector(i);
    }
    void SetVector(int i, Vec128 v) override {
        native_ ? void(st_.v[i] = {v[0], v[1]}) : jit_->SetVector(i, v);
    }
    u32 Fpcr() const override { return native_ ? st_.fpcr : jit_->GetFpcr(); }
    void SetFpcr(u32 v) override { native_ ? void(st_.fpcr = v) : jit_->SetFpcr(v); }
    u32 Fpsr() const override { return native_ ? st_.fpsr : jit_->GetFpsr(); }
    void SetFpsr(u32 v) override { native_ ? void(st_.fpsr = v) : jit_->SetFpsr(v); }
    u32 Pstate() const override { return native_ ? (pstate_rest_ | st_.nzcv) : jit_->GetPstate(); }
    void SetPstate(u32 v) override {
        if (native_) {
            st_.nzcv = v & 0xF0000000u;
            pstate_rest_ = v & 0x0FFFFFFFu;
        } else {
            jit_->SetPstate(v);
        }
    }
    u64 TpidrEl0() const override { return native_ ? st_.tpidr : tpidr_el0_; }
    void SetTpidrEl0(u64 v) override { native_ ? void(st_.tpidr = v) : void(tpidr_el0_ = v); }
    void SetTpidrroEl0(u64 v) override { tpidrro_el0_ = v; }
    void InvalidateCache(u64 addr, u64 size) override { jit_->InvalidateCacheRange(addr, size); }
    void ClearExclusive() override { jit_->ClearExclusiveState(); }

    void RunOnCore(std::function<void()> fn) override {
        {
            std::lock_guard lk(inv_mutex_);
            callbacks_.push_back(std::move(fn));
        }
        native_halt_ = 1;
        jit_->HaltExecution(kHaltCallback);
    }
    void QueueInvalidate(u64 addr, u64 size) {
        {
            std::lock_guard lk(inv_mutex_);
            pending_.emplace_back(addr, size);
        }
        native_halt_ = 1;
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
        // Entries of recompiled functions decode as BRK so that the JIT traps into RunNative.
        if (NativeAt(vaddr))
            return kTrapWord;
        return mem_.Read<u32>(vaddr);
    }
    recomp::Fn NativeAt(u64 pc) const;
    void RunNative(recomp::Fn fn, u64 pc);
    static constexpr u32 kTrapWord = 0xD4200000u | (0xDEADu << 5); // BRK #0xdead
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
        if (auto fn = NativeAt(pc)) {
            RunNative(fn, pc);
            return;
        }
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
    void AddTicks(u64 ticks) override;
    u64 GetTicksRemaining() override { return 200000; }
    u64 GetCNTPCT() override { return cb_.cntpct(); }

    size_t slot_;
    recomp::State st_{};
    volatile u32 native_halt_ = 0;
    bool native_ = false;
    u32 pstate_rest_ = 0;
    std::atomic<bool> sample_pending_{false};
    u64 exit_count_ = 0;

    // Host code runs with the default MXCSR; native guest code with one mirroring FPCR (see RunNative).
    void Svc(u32 imm) {
        if (guest_mxcsr_ != host_mxcsr_)
            _mm_setcsr(host_mxcsr_);
        cb_.svc(imm);
        if (guest_mxcsr_ != host_mxcsr_)
            _mm_setcsr(guest_mxcsr_);
    }
    u32 host_mxcsr_ = 0, guest_mxcsr_ = 0;
    u64 Cntpct() { return cb_.cntpct(); }
    Dynarmic::ExclusiveMonitor& Monitor() { return monitor_; }
    DynarmicFactory& Factory() { return factory_; }

private:
    DynarmicFactory& factory_;
    core::Memory& mem_;
    Dynarmic::ExclusiveMonitor& monitor_;
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
    DynarmicFactory(core::Memory& mem, size_t max_cores) : mem_(mem), monitor_(max_cores), max_(max_cores) {
        const char* env = std::getenv("MUA3_RECOMP");
        native_enabled_ = !(env && env[0] == '0');
        if (const char* p = std::getenv("MUA3_RECOMP_PROFILE"))
            StartSampler(p);
        profiling_ = std::getenv("MUA3_RECOMP_PROFILE") != nullptr;
    }
    ~DynarmicFactory() override {
        sampler_stop_ = true;
        if (sampler_.joinable())
            sampler_.join();
    }

    // ---- recompiled code
    // Verified lazily on first use (after module load and code patches) and again for ranges that get invalidated.
    void EnsureNativeTable() {
        std::call_once(table_once_, [this] {
            const auto& t = recomp::GetTable();
            size_t ok = 0;
            for (u32 i = 0; i < t.num_funcs; i++) {
                if (!t.funcs[i].fn)
                    continue;
                t.ok[i] = native_enabled_ && Verify(t, i);
                ok += t.ok[i];
            }
            std::unique_lock lk(native_mutex_);
            for (u32 i = 0; i < t.num_funcs; i++)
                if (t.ok[i])
                    native_.emplace(t.funcs[i].entry, t.funcs[i].fn);
            size_t resumes = 0;
            for (u32 i = 0; i < t.num_resumes; i++)
                if (t.ok[t.resumes[i].func])
                    resumes += native_.emplace(t.resumes[i].pc, t.funcs[t.resumes[i].func].fn).second;
            LOG_INFO("Recomp", "{} of {} recompiled functions enabled, {} resume points{}", ok, t.num_funcs, resumes,
                     native_enabled_ ? "" : " (MUA3_RECOMP=0)");
            Publish();
            table_ready_ = true;
        });
    }
    bool Verify(const recomp::Table& t, u32 i) {
        const auto& f = t.funcs[i];
        u64 h = 0xcbf29ce484222325ull;
        for (u32 r = f.range_first; r < f.range_first + f.range_count; r++) {
            for (u64 a = t.ranges[r].start; a < t.ranges[r].end; a += 4) {
                if (!mem_.IsValid(a, 4))
                    return false;
                const u32 w = mem_.Read<u32>(a);
                for (int k = 0; k < 4; k++) {
                    h ^= (w >> (k * 8)) & 0xFF;
                    h *= 0x100000001b3ull;
                }
            }
        }
        return h == f.hash;
    }
    void RecheckNative(u64 addr, u64 size) {
        const auto& t = recomp::GetTable();
        std::unique_lock lk(native_mutex_);
        bool changed = false;
        for (u32 i = 0; i < t.num_funcs; i++) {
            if (!t.ok[i])
                continue;
            const auto& f = t.funcs[i];
            for (u32 r = f.range_first; r < f.range_first + f.range_count; r++) {
                if (t.ranges[r].start < addr + size && addr < t.ranges[r].end) {
                    if (!Verify(t, i)) {
                        t.ok[i] = 0;
                        native_.erase(f.entry);
                        for (u32 k = 0; k < t.num_resumes; k++)
                            if (t.resumes[k].func == i)
                                native_.erase(t.resumes[k].pc);
                        changed = true;
                        LOG_INFO("Recomp", "code at {:#x} changed: function {:#x} back to the JIT", addr, f.entry);
                    }
                    break;
                }
            }
        }
        if (changed)
            Publish();
    }
    void DisableNative(u64 pc) {
        {
            std::unique_lock lk(native_mutex_);
            if (!native_.erase(pc))
                return;
            Publish();
        }
        std::lock_guard lk(mutex_);
        for (auto* c : cores_)
            c->QueueInvalidate(pc, 4);
    }
    // Lock-free lookup: an immutable open-addressing table, replaced (old one leaked) when native_ changes.
    struct NativeMap {
        u64 mask;
        std::vector<std::pair<u64, recomp::Fn>> slots;
    };
    static u64 Slot(u64 pc) { return (pc >> 2) * 0x9E3779B97F4A7C15ull >> 20; }
    void Publish() {
        auto* m = new NativeMap;
        u64 size = 1024;
        while (size < native_.size() * 4)
            size *= 2;
        m->mask = size - 1;
        m->slots.assign(size, {0, nullptr});
        for (auto [pc, fn] : native_) {
            u64 i = Slot(pc) & m->mask;
            while (m->slots[i].first)
                i = (i + 1) & m->mask;
            m->slots[i] = {pc, fn};
        }
        map_.store(m, std::memory_order_release);
    }
    recomp::Fn Lookup(u64 pc) const {
        const NativeMap* m = map_.load(std::memory_order_acquire);
        if (!m)
            return nullptr;
        for (u64 i = Slot(pc) & m->mask;; i = (i + 1) & m->mask) {
            const auto& e = m->slots[i];
            if (e.first == pc)
                return e.second;
            if (!e.first)
                return nullptr;
        }
    }
    std::atomic<u64> transitions_{0};
    bool TableReady() const { return table_ready_.load(std::memory_order_acquire); }

    // ---- coverage: fraction of guest instructions executed by recompiled code
    std::atomic<u64> native_instr_{0}, jit_instr_{0};
    std::atomic<s64> last_report_{0};
    void MaybeReport() {
        const s64 now = std::chrono::duration_cast<std::chrono::seconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count();
        s64 last = last_report_.load(std::memory_order_relaxed);
        if (now - last < 10 || !last_report_.compare_exchange_strong(last, now))
            return;
        const u64 n = native_instr_.exchange(0), j = jit_instr_.exchange(0), tr = transitions_.exchange(0);
        if (n + j)
            LOG_INFO("Recomp", "native {:.1f}% of {} M guest instructions, {} k JIT->native entries (last 10 s)",
                     100.0 * double(n) / double(n + j), (n + j) / 1000000, tr / 1000);
    }

    // ---- PC sampling profile for the recompiler (MUA3_RECOMP_PROFILE=<file>)
    bool profiling_ = false;
    void RecordSample(u64 pc, u64 n) {
        std::lock_guard sl(sample_mutex_);
        samples_[pc] += n;
    }
    void StartSampler(std::string path) {
        sampler_ = std::thread([this, path] {
            auto last_dump = std::chrono::steady_clock::now();
            while (!sampler_stop_) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                {
                    std::lock_guard lk(mutex_);
                    for (auto* c : cores_) {
                        if (c->sample_pending_.exchange(true))
                            continue;
                        c->RunOnCore([this, c] {
                            c->sample_pending_ = false;
                            std::lock_guard sl(sample_mutex_);
                            samples_[c->PC()]++;
                        });
                    }
                }
                if (std::chrono::steady_clock::now() - last_dump > std::chrono::seconds(10)) {
                    last_dump = std::chrono::steady_clock::now();
                    std::lock_guard sl(sample_mutex_);
                    std::ofstream f(path, std::ios::trunc);
                    for (auto [pc, n] : samples_)
                        f << std::hex << pc << " " << std::dec << n << "\n";
                }
            }
        });
    }

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
        if (table_ready_)
            RecheckNative(addr, size);
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
    bool native_enabled_ = true;
    std::atomic<bool> table_ready_{false};
    std::once_flag table_once_;
    std::shared_mutex native_mutex_;
    std::atomic<NativeMap*> map_{nullptr};
    std::unordered_map<u64, recomp::Fn> native_;
    std::thread sampler_;
    std::atomic<bool> sampler_stop_{false};
    std::mutex sample_mutex_;
    std::unordered_map<u64, u64> samples_;
};

DynarmicCore::DynarmicCore(DynarmicFactory& f, core::Memory& mem, Dynarmic::ExclusiveMonitor& monitor,
                           CpuCallbacks cb, size_t slot)
    : slot_(slot), factory_(f), mem_(mem), monitor_(monitor), cb_(std::move(cb)) {
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

recomp::Fn DynarmicCore::NativeAt(u64 pc) const {
    if (!factory_.TableReady()) [[unlikely]]
        factory_.EnsureNativeTable();
    return factory_.Lookup(pc);
}

void DynarmicCore::RunNative(recomp::Fn fn, u64 pc) {
    auto& s = st_;
    s.pc = pc;
    auto regs = jit_->GetRegisters();
    std::memcpy(s.x, regs.data(), sizeof(s.x));
    auto vecs = jit_->GetVectors();
    std::memcpy(s.v, vecs.data(), sizeof(s.v));
    s.sp = jit_->GetSP();
    const u32 pstate = jit_->GetPstate();
    s.nzcv = pstate & 0xF0000000u;
    pstate_rest_ = pstate & 0x0FFFFFFFu;
    s.fpcr = jit_->GetFpcr();
    s.fpsr = jit_->GetFpsr();
    s.tpidr = tpidr_el0_;
    s.tpidrro = tpidrro_el0_;
    s.mem = mem_.Base();
    s.halt = &native_halt_;
    s.core = this;
    if (!s.stack_limit) {
        ULONG_PTR lo, hi;
        GetCurrentThreadStackLimits(&lo, &hi);
        s.stack_limit = u64(lo) + (256u << 10);
    }
    const u64 icount0 = s.icount;
    factory_.transitions_.fetch_add(1, std::memory_order_relaxed);
    static std::atomic<bool> fpcr_logged{false};
    if (!fpcr_logged.exchange(true))
        LOG_INFO("Recomp", "first native entry {:#x}, guest FPCR {:#x}", s.pc, s.fpcr);
    native_ = true;
    // FPCR RMode (23:22: RN, RP, RM, RZ) -> MXCSR RC (14:13: RN, RM, RP, RZ); FZ -> FTZ + DAZ
    static constexpr u32 kRc[4] = {0u << 13, 2u << 13, 1u << 13, 3u << 13};
    host_mxcsr_ = _mm_getcsr();
    guest_mxcsr_ = (host_mxcsr_ & ~0xE040u) | kRc[(s.fpcr >> 22) & 3] | ((s.fpcr >> 24) & 1 ? 0x8040u : 0u);
    if (guest_mxcsr_ != host_mxcsr_)
        _mm_setcsr(guest_mxcsr_);
    // Chain native functions (tail jumps, returns into other entries) until the guest leaves recompiled code.
    for (;;) {
        const u64 before = s.icount;
        fn(s);
        // no progress (stack floor, disabled function): let the JIT take over
        if (s.icount == before) {
            static std::atomic<int> reports{0};
            if (reports++ < 10)
                LOG_WARN("Recomp", "no progress at {:#x} (pc after {:#x}, stack {:#x}, floor {:#x}, halt {})", pc,
                         s.pc, reinterpret_cast<u64>(_AddressOfReturnAddress()), s.stack_limit, native_halt_);
            // Never trap here again, or the JIT and native code would hand this pc back and forth forever.
            if (s.pc == pc && !native_halt_)
                factory_.DisableNative(pc);
            break;
        }
        if (native_halt_ || !(fn = factory_.Lookup(s.pc)))
            break;
    }
    if (guest_mxcsr_ != host_mxcsr_)
        _mm_setcsr(host_mxcsr_);
    // Profile where the JIT takes over (unsupported block, callee not recompiled, return into a JIT caller).
    if (factory_.profiling_ && (++exit_count_ & 63) == 0)
        factory_.RecordSample(s.pc, 64);
    native_ = false;
    factory_.native_instr_.fetch_add(s.icount - icount0, std::memory_order_relaxed);

    std::memcpy(regs.data(), s.x, sizeof(s.x));
    jit_->SetRegisters(regs);
    std::memcpy(vecs.data(), s.v, sizeof(s.v));
    jit_->SetVectors(vecs);
    jit_->SetSP(s.sp);
    jit_->SetPstate(pstate_rest_ | s.nzcv);
    jit_->SetFpcr(s.fpcr);
    jit_->SetFpsr(s.fpsr);
    tpidr_el0_ = s.tpidr;
    jit_->SetPC(s.pc);
}

void DynarmicCore::AddTicks(u64 ticks) {
    factory_.jit_instr_.fetch_add(ticks, std::memory_order_relaxed);
    factory_.MaybeReport();
}

} // namespace

std::unique_ptr<CoreFactory> CreateDynarmicFactory(core::Memory& memory, size_t max_cores) {
    return std::make_unique<DynarmicFactory>(memory, max_cores);
}

} // namespace cpu

// ---- runtime services called from recompiled code
namespace recomp {
namespace {
cpu::DynarmicCore& CoreOf(State& s) { return *static_cast<cpu::DynarmicCore*>(s.core); }
template <typename T>
T ExRead(State& s, u64 a) {
    auto& c = CoreOf(s);
    return c.Monitor().ReadAndMark<T>(c.slot_, a, [&] { return Rd<T>(s, a); });
}
template <typename T>
u32 ExWrite(State& s, u64 a, T v) {
    auto& c = CoreOf(s);
    return c.Monitor().DoExclusiveOperation<T>(c.slot_, a,
                                               [&](T expected) {
                                                   return std::atomic_ref<T>(*reinterpret_cast<T*>(s.mem + a))
                                                       .compare_exchange_strong(expected, v);
                                               })
               ? 0
               : 1;
}
} // namespace

void Svc(State& s, u32 imm) { CoreOf(s).Svc(imm); }
u64 Cntpct(State& s) { return CoreOf(s).Cntpct(); }
void Barrier() { std::atomic_thread_fence(std::memory_order_seq_cst); }
u8 ExRead8(State& s, u64 a) { return ExRead<u8>(s, a); }
u16 ExRead16(State& s, u64 a) { return ExRead<u16>(s, a); }
u32 ExRead32(State& s, u64 a) { return ExRead<u32>(s, a); }
u64 ExRead64(State& s, u64 a) { return ExRead<u64>(s, a); }
V128 ExRead128(State& s, u64 a) {
    auto& c = CoreOf(s);
    const auto v = c.Monitor().ReadAndMark<Dynarmic::A64::Vector>(c.slot_, a, [&] {
        const V128 r = Rd128(s, a);
        return Dynarmic::A64::Vector{r.lo, r.hi};
    });
    return {v[0], v[1]};
}
u32 ExWrite8(State& s, u64 a, u8 v) { return ExWrite<u8>(s, a, v); }
u32 ExWrite16(State& s, u64 a, u16 v) { return ExWrite<u16>(s, a, v); }
u32 ExWrite32(State& s, u64 a, u32 v) { return ExWrite<u32>(s, a, v); }
u32 ExWrite64(State& s, u64 a, u64 v) { return ExWrite<u64>(s, a, v); }
u32 ExWrite128(State& s, u64 a, V128 v) {
    auto& c = CoreOf(s);
    return c.Monitor().DoExclusiveOperation<Dynarmic::A64::Vector>(
               c.slot_, a,
               [&](Dynarmic::A64::Vector e) {
                   long long cmp[2] = {(long long)e[0], (long long)e[1]};
                   return _InterlockedCompareExchange128(reinterpret_cast<long long*>(s.mem + a), (long long)v.hi,
                                                         (long long)v.lo, cmp) != 0;
               })
               ? 0
               : 1;
}
void ClearExclusive(State& s) { CoreOf(s).Monitor().ClearProcessor(CoreOf(s).slot_); }
void Dispatch(State& s) {
    if (auto fn = CoreOf(s).Factory().Lookup(s.pc))
        fn(s);
}
} // namespace recomp
