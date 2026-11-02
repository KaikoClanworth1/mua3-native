#include <atomic>
#include <chrono>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <mutex>
#include <set>
#include <shared_mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include <windows.h>

#include <dynarmic/interface/A64/a64.h>
#include <dynarmic/interface/A64/config.h>
#include <dynarmic/interface/exclusive_monitor.h>

#include "core/cpu/cpu.h"
#include "core/cpu/recomp/recomp_rt.h"
#include "core/hle/nvn/nvn_hle.h"
#include "core/memory.h"

namespace cpu {

namespace {

constexpr auto kHaltStop = Dynarmic::HaltReason::UserDefined1;
constexpr auto kHaltInvalidate = Dynarmic::HaltReason::UserDefined2;
constexpr auto kHaltCallback = Dynarmic::HaltReason::UserDefined3;
constexpr auto kHaltNative = Dynarmic::HaltReason::UserDefined4; // reached a recompiled entry

class DynarmicFactory;

class DynarmicCore final : public Core, public Dynarmic::A64::UserCallbacks {
public:
    DynarmicCore(DynarmicFactory& f, core::Memory& mem, Dynarmic::ExclusiveMonitor& monitor, CpuCallbacks cb,
                 size_t slot);
    ~DynarmicCore() override;

    // ---- Core
    // Guest threads run recompiled code directly; the register file lives in st_. Only when a thread reaches code
    // without a native version does it get a dynarmic JIT (created on first need: each one commits ~18 MB, and the
    // game creates hundreds of short-lived threads), which runs until it reaches native code again.
    void Run() override {
        stop_ = false;
        while (!stop_) {
            native_halt_ = 0;
            std::atomic_thread_fence(std::memory_order_seq_cst);
            ServiceRequests();
            if (stop_)
                break;
            if (recomp::Fn fn = NativeAt(st_.pc)) {
                RunNativeChain(fn);
                MaybeReport();
            } else {
                RunJit(false);
            }
        }
    }
    void Halt() override {
        stop_ = true;
        RequestHalt(kHaltStop);
    }
    void Step() override { RunJit(true); }
    // The register file lives in st_ except while the JIT runs non-native code.
    bool JitOwns() const { return in_jit_ && !native_; }
    u64 Reg(int i) const override { return JitOwns() ? jit_->GetRegister(i) : st_.x[i]; }
    void SetReg(int i, u64 v) override { JitOwns() ? jit_->SetRegister(i, v) : void(st_.x[i] = v); }
    u64 PC() const override { return JitOwns() ? jit_->GetPC() : st_.pc; }
    void SetPC(u64 v) override { JitOwns() ? jit_->SetPC(v) : void(st_.pc = v); }
    u64 SP() const override { return JitOwns() ? jit_->GetSP() : st_.sp; }
    void SetSP(u64 v) override { JitOwns() ? jit_->SetSP(v) : void(st_.sp = v); }
    Vec128 Vector(int i) const override {
        return JitOwns() ? jit_->GetVector(i) : Vec128{st_.v[i].lo, st_.v[i].hi};
    }
    void SetVector(int i, Vec128 v) override {
        JitOwns() ? jit_->SetVector(i, v) : void(st_.v[i] = {v[0], v[1]});
    }
    u32 Fpcr() const override { return JitOwns() ? jit_->GetFpcr() : st_.fpcr; }
    void SetFpcr(u32 v) override { JitOwns() ? jit_->SetFpcr(v) : void(st_.fpcr = v); }
    u32 Fpsr() const override { return JitOwns() ? jit_->GetFpsr() : st_.fpsr; }
    void SetFpsr(u32 v) override { JitOwns() ? jit_->SetFpsr(v) : void(st_.fpsr = v); }
    u32 Pstate() const override { return JitOwns() ? jit_->GetPstate() : (pstate_rest_ | st_.nzcv); }
    void SetPstate(u32 v) override {
        if (JitOwns()) {
            jit_->SetPstate(v);
        } else {
            st_.nzcv = v & 0xF0000000u;
            pstate_rest_ = v & 0x0FFFFFFFu;
        }
    }
    u64 TpidrEl0() const override { return JitOwns() ? tpidr_el0_ : st_.tpidr; }
    void SetTpidrEl0(u64 v) override { JitOwns() ? void(tpidr_el0_ = v) : void(st_.tpidr = v); }
    void SetTpidrroEl0(u64 v) override { tpidrro_el0_ = st_.tpidrro = v; }
    void InvalidateCache(u64 addr, u64 size) override {
        if (jit_)
            jit_->InvalidateCacheRange(addr, size);
    }
    void ClearExclusive() override {
        if (jit_)
            jit_->ClearExclusiveState();
        else
            monitor_.ClearProcessor(slot_);
    }

    // Makes the thread leave native code (next back edge / SVC return) and the JIT (next block) and come back to
    // Run(). Pairs with the fence in RunJit: either the JIT sees native_halt_ before it starts, or we see its JIT.
    void RequestHalt(Dynarmic::HaltReason why) {
        native_halt_ = 1;
        std::atomic_thread_fence(std::memory_order_seq_cst);
        if (auto* j = jit_ptr_.load(std::memory_order_seq_cst))
            j->HaltExecution(why);
    }
    void RunOnCore(std::function<void()> fn) override {
        {
            std::lock_guard lk(inv_mutex_);
            callbacks_.push_back(std::move(fn));
        }
        RequestHalt(kHaltCallback);
    }
    void QueueInvalidate(u64 addr, u64 size) {
        {
            std::lock_guard lk(inv_mutex_);
            pending_.emplace_back(addr, size);
        }
        RequestHalt(kHaltInvalidate);
    }
    void ServiceRequests() {
        std::vector<std::function<void()>> fns;
        {
            std::lock_guard lk(inv_mutex_);
            fns.swap(callbacks_);
            if (jit_)
                for (auto [a, s] : pending_)
                    jit_->InvalidateCacheRange(a, s);
            pending_.clear(); // without a JIT there is no translated code to drop
        }
        for (auto& f : fns)
            f();
    }
    void EnsureJit();
    void RunJit(bool single_step);
    void RunNativeChain(recomp::Fn fn);
    void MaybeReport();

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
        if (NativeAt(pc)) { // the JIT reached native code: Run() continues there
            jit_->SetPC(pc);
            jit_->HaltExecution(kHaltNative);
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
  public:
    std::atomic<u64> instr_window_{0}; // native guest instructions since the last report (per-thread table)
    u32 host_tid_ = 0;

    // Host code runs with the default MXCSR; native guest code with one mirroring FPCR (see RunNative).
    void Svc(u32 imm) {
        if (guest_mxcsr_ != host_mxcsr_)
            _mm_setcsr(host_mxcsr_);
        cb_.svc(imm);
        if (guest_mxcsr_ != host_mxcsr_)
            _mm_setcsr(guest_mxcsr_);
    }
    u32 host_mxcsr_ = 0, guest_mxcsr_ = 0;
    // FPCR RMode (23:22: RN, RP, RM, RZ) -> MXCSR RC (14:13: RN, RM, RP, RZ); FZ -> FTZ + DAZ
    u32 GuestMxcsr(u32 fpcr) const {
        static constexpr u32 kRc[4] = {0u << 13, 2u << 13, 1u << 13, 3u << 13};
        return (host_mxcsr_ & ~0xE040u) | kRc[(fpcr >> 22) & 3] | ((fpcr >> 24) & 1 ? 0x8040u : 0u);
    }
    void NativeSetFpcr(u32 v) { // MSR FPCR inside recompiled code
        st_.fpcr = v;
        guest_mxcsr_ = GuestMxcsr(v);
        _mm_setcsr(guest_mxcsr_);
    }
    u64 Cntpct() { return cb_.cntpct(); }
    Dynarmic::ExclusiveMonitor& Monitor() { return monitor_; }
    DynarmicFactory& Factory() { return factory_; }

private:
    DynarmicFactory& factory_;
    core::Memory& mem_;
    Dynarmic::ExclusiveMonitor& monitor_;
    CpuCallbacks cb_;
    u64 tpidr_el0_ = 0, tpidrro_el0_ = 0;
    std::unique_ptr<Dynarmic::A64::Jit> jit_;   // created on first need (EnsureJit)
    std::atomic<Dynarmic::A64::Jit*> jit_ptr_{nullptr}; // the same, for halt requests from other threads
    bool in_jit_ = false;
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
                *t.ok[i] = native_enabled_ && Verify(t, i);
                ok += *t.ok[i];
            }
            std::unique_lock lk(native_mutex_);
            for (u32 i = 0; i < t.num_funcs; i++)
                if (*t.ok[i])
                    native_.emplace(t.funcs[i].entry, t.funcs[i].fn);
            size_t resumes = 0;
            for (u32 i = 0; i < t.num_resumes; i++)
                if (*t.ok[t.resumes[i].func])
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
            if (!*t.ok[i])
                continue;
            const auto& f = t.funcs[i];
            for (u32 r = f.range_first; r < f.range_first + f.range_count; r++) {
                if (t.ranges[r].start < addr + size && addr < t.ranges[r].end) {
                    if (!Verify(t, i)) {
                        *t.ok[i] = 0;
                        native_.erase(f.entry);
                        SetSlot(f.entry, nullptr);
                        for (u32 k = 0; k < t.num_resumes; k++)
                            if (t.resumes[k].func == i) {
                                native_.erase(t.resumes[k].pc);
                                SetSlot(t.resumes[k].pc, nullptr);
                            }
                        changed = true;
                        LOG_INFO("Recomp", "code at {:#x} changed: function {:#x} back to the JIT", addr, f.entry);
                    }
                    break;
                }
            }
        }
        (void)changed;
    }
    void DisableNative(u64 pc) {
        {
            std::unique_lock lk(native_mutex_);
            if (!native_.erase(pc))
                return;
            SetSlot(pc, nullptr);
        }
        std::lock_guard lk(mutex_);
        for (auto* c : cores_)
            c->QueueInvalidate(pc, 4);
    }
    // Lock-free lookup: an open-addressing table whose keys never change once published. Changing or removing the
    // entry of a pc already in the table only swaps its function pointer (nullptr = not native); only a new pc
    // rebuilds the table. The full table is ~64 MB (700k entries), so replaced tables are freed after a grace
    // period instead of being leaked: every NVN override used to leak one (11 GB at boot).
    struct NativeSlot {
        u64 pc = 0;
        std::atomic<recomp::Fn> fn{nullptr};
    };
    struct NativeMap {
        u64 mask = 0;
        std::unique_ptr<NativeSlot[]> slots;
    };
    static u64 Slot(u64 pc) { return (pc >> 2) * 0x9E3779B97F4A7C15ull >> 20; }
    static NativeSlot* Find(const NativeMap* m, u64 pc) {
        for (u64 i = Slot(pc) & m->mask;; i = (i + 1) & m->mask) {
            NativeSlot& e = m->slots[i];
            if (e.pc == pc)
                return &e;
            if (!e.pc)
                return nullptr;
        }
    }
    // native_mutex_ held. Brings the published table in line with native_[pc] (fn == nullptr: removed).
    void SetSlot(u64 pc, recomp::Fn fn) {
        if (NativeMap* m = map_.load(std::memory_order_relaxed))
            if (NativeSlot* e = Find(m, pc)) {
                e->fn.store(fn, std::memory_order_release);
                return;
            }
        if (fn)
            Publish();
    }
    void Publish() { // native_mutex_ held
        auto* m = new NativeMap;
        u64 size = 1024;
        while (size < native_.size() * 4)
            size *= 2;
        m->mask = size - 1;
        m->slots.reset(new NativeSlot[size]);
        for (auto [pc, fn] : native_) {
            u64 i = Slot(pc) & m->mask;
            while (m->slots[i].pc)
                i = (i + 1) & m->mask;
            m->slots[i].pc = pc;
            m->slots[i].fn.store(fn, std::memory_order_relaxed);
        }
        NativeMap* old = map_.exchange(m, std::memory_order_acq_rel);
        // Readers only hold a table for the duration of one Lookup: free replaced ones once they are 10 s old.
        const auto now = std::chrono::steady_clock::now();
        if (old)
            retired_.emplace_back(now, old);
        while (!retired_.empty() && now - retired_.front().first > std::chrono::seconds(10)) {
            delete retired_.front().second;
            retired_.pop_front();
        }
    }
    recomp::Fn Lookup(u64 pc) const {
        const NativeMap* m = map_.load(std::memory_order_acquire);
        if (!m)
            return nullptr;
        const NativeSlot* e = Find(m, pc);
        return e ? e->fn.load(std::memory_order_acquire) : nullptr;
    }
    std::atomic<u64> transitions_{0};
    std::atomic<u32> jit_cores_{0}; // guest threads that ever needed the JIT (since boot)
    bool TableReady() const { return table_ready_.load(std::memory_order_acquire); }

    // ---- coverage: fraction of guest instructions executed by recompiled code
    std::atomic<u64> native_instr_{0}, jit_instr_{0};
    std::atomic<s64> last_report_{0};
    // MUA3_NVN_CENSUS=1: count indirect calls made by recompiled code per target (the game calls every NVN
    // function through a pointer) and write them, with the game's NVN pointer table, to nvn_census.txt every 10 s.
    const bool census_ = std::getenv("MUA3_NVN_CENSUS") != nullptr;
    std::mutex census_mutex_;
    std::unordered_map<u64, u64> census_calls_;
    std::chrono::steady_clock::time_point census_start_ = std::chrono::steady_clock::now();
    std::unordered_map<u64, std::string> census_names_;
    void Census(u64 pc) {
        std::lock_guard lk(census_mutex_);
        census_calls_[pc]++;
    }
    // nvnDeviceGetProcAddress results (see recomp::Dispatch)
    std::atomic<bool> capture_names_{true};
    std::set<u64> proc_addr_pcs_; // lookup functions (nvnDeviceGetProcAddress and friends), once identified
    size_t names_seen_ = 0;
    std::set<std::string> unique_names_;
    std::unordered_map<u64, std::vector<std::pair<u64, std::string>>> name_candidates_; // per calling target
    // Returns true once, when the last NVN name has been resolved.
    bool OnProcAddress(u64 getproc_pc, u64 ptr, const char* name) {
        bool done = false;
        // Other functions also take "nvn..." strings (string compares and the like): only results that point at
        // recompiled game code are entry points.
        if (!Lookup(ptr))
            return false;
        std::vector<std::pair<u64, std::string>> apply;
        {
            std::lock_guard lk(census_mutex_);
            if (census_)
                census_names_.emplace(ptr, name);
            if (!proc_addr_pcs_.count(getproc_pc)) {
                // Other calls can pass an "nvn..." string too: a lookup function is one that resolves several names.
                auto& c = name_candidates_[getproc_pc];
                c.emplace_back(ptr, name);
                if (c.size() < 4)
                    return false;
                proc_addr_pcs_.insert(getproc_pc);
                apply = std::move(c);
                name_candidates_.erase(getproc_pc);
                LOG_INFO("NVN", "NVN name lookup function at {:#x}", getproc_pc);
            } else {
                apply.emplace_back(ptr, name);
            }
            for (const auto& a : apply)
                unique_names_.insert(a.second);
            names_seen_ = unique_names_.size();
            if (names_seen_ >= 519 && capture_names_) { // the game resolves 519 names (some of them twice)
                capture_names_ = false;
                done = true;
                LOG_INFO("NVN", "{} NVN entry points resolved", names_seen_);
            }
        }
        for (const auto& [p, n] : apply)
            OverrideWithHost(p, n.c_str());
        return done;
    }
    void OverrideWithHost(u64 ptr, const char* name) {
        if (auto host = hle::nvn::Find(name, Lookup(ptr), ptr); host && ptr && host != Lookup(ptr)) {
            {
                std::unique_lock lk(native_mutex_);
                native_[ptr] = host;
                SetSlot(ptr, host);
            }
            std::lock_guard lk(mutex_);
            for (auto* c : cores_)
                c->QueueInvalidate(ptr, 4); // JIT callers trap into the host function too
            LOG_INFO("NVN", "{} -> host implementation (driver function {:#x})", name, ptr);
        }
    }
    bool GuestValid(u64 a, u64 n) const { return mem_.IsValid(a, n); }
    void DumpCensus() {
        // The NVN entry points main resolves by name land in one array of function pointers into the sdk module
        // (the NVN driver). This build's layout: main data/bss 0xa05c000..0xcfc5000, sdk text 0xdf54000+0x51c600.
        constexpr u64 kScanBegin = 0x8004000 + 0x2058000, kScanEnd = 0x8004000 + 0x4fc1000;
        constexpr u64 kSdkBegin = 0xdf54000, kSdkEnd = 0xdf54000 + 0x51c600;
        u64 best = 0, best_len = 0, run = 0, len = 0;
        for (u64 a = kScanBegin; a + 8 <= kScanEnd; a += 8) {
            const u64 v = mem_.IsValid(a, 8) ? mem_.Read<u64>(a) : 0;
            if (v >= kSdkBegin && v < kSdkEnd) {
                if (!len)
                    run = a;
                if (++len > best_len)
                    best = run, best_len = len;
            } else {
                len = 0;
            }
        }
        std::ofstream f("nvn_census.txt", std::ios::trunc);
        f << "seconds " << std::chrono::duration<double>(std::chrono::steady_clock::now() - census_start_).count()
          << "\ntable " << std::hex << best << " " << std::dec << best_len << "\n";
        for (u64 i = 0; i < best_len; i++)
            f << "slot " << i << " " << std::hex << mem_.Read<u64>(best + i * 8) << std::dec << "\n";
        std::lock_guard lk(census_mutex_);
        for (const auto& [ptr, name] : census_names_)
            f << "name " << std::hex << ptr << std::dec << " " << name << "\n";
        for (auto [pc, n] : census_calls_)
            f << "call " << std::hex << pc << " " << std::dec << n << "\n";
    }
    void MaybeReport() {
        const s64 now = std::chrono::duration_cast<std::chrono::seconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count();
        s64 last = last_report_.load(std::memory_order_relaxed);
        if (now - last < 10 || !last_report_.compare_exchange_strong(last, now))
            return;
        if (census_)
            DumpCensus();
        const u64 n = native_instr_.exchange(0), j = jit_instr_.exchange(0), tr = transitions_.exchange(0);
        if (n + j)
            g_native_percent = float(100.0 * double(n) / double(n + j));
        if (n + j)
            LOG_INFO("Recomp", "native {:.2f}% of {} M guest instructions, {} k native entries (last 10 s); "
                     "{} threads have needed the JIT since boot",
                     100.0 * double(n) / double(n + j), (n + j) / 1000000, tr / 1000, jit_cores_.load());
        // the busiest guest threads (host thread ids), for comparing per-thread work between runs
        std::vector<std::pair<u64, u32>> per;
        {
            std::lock_guard lk(mutex_);
            for (auto* c : cores_)
                per.emplace_back(c->instr_window_.exchange(0), c->host_tid_);
        }
        std::sort(per.rbegin(), per.rend());
        std::string top;
        for (size_t i = 0; i < per.size() && i < 5; i++)
            top += fmt::format(" {}:{}M", per[i].second, per[i].first / 1000000);
        LOG_INFO("Recomp", "guest instructions by thread (last 10 s):{}", top);
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
    std::deque<std::pair<std::chrono::steady_clock::time_point, NativeMap*>> retired_;
    std::unordered_map<u64, recomp::Fn> native_;
    std::thread sampler_;
    std::atomic<bool> sampler_stop_{false};
    std::mutex sample_mutex_;
    std::unordered_map<u64, u64> samples_;
};

DynarmicCore::DynarmicCore(DynarmicFactory& f, core::Memory& mem, Dynarmic::ExclusiveMonitor& monitor,
                           CpuCallbacks cb, size_t slot)
    : slot_(slot), factory_(f), mem_(mem), monitor_(monitor), cb_(std::move(cb)) {}

DynarmicCore::~DynarmicCore() { factory_.Release(this); }

void DynarmicCore::EnsureJit() {
    if (jit_)
        return;
    Dynarmic::A64::UserConfig c{};
    c.callbacks = this;
    c.processor_id = slot_;
    c.global_monitor = &monitor_;
    c.tpidrro_el0 = &tpidrro_el0_;
    c.tpidr_el0 = &tpidr_el0_;
    c.cntfrq_el0 = 19200000;
    c.dczid_el0 = 4;
    c.ctr_el0 = 0x8444c004;
    c.fastmem_pointer = reinterpret_cast<uintptr_t>(mem_.Base());
    c.fastmem_address_space_bits = core::layout::kSpaceBits;
    c.silently_mirror_fastmem = false;
    c.fastmem_exclusive_access = true;
    c.recompile_on_fastmem_failure = true;
    c.wall_clock_cntpct = true;
    c.enable_cycle_counting = true; // return to the dispatcher regularly so halts are always seen
    c.define_unpredictable_behaviour = true;
    // Per guest thread; reserved, committed as used. MUA3_JIT_CACHE_MB overrides.
    static const u64 cache_mb = [] {
        const char* e = std::getenv("MUA3_JIT_CACHE_MB");
        return e ? std::max<u64>(std::strtoull(e, nullptr, 10), 2) : 128ull;
    }();
    c.code_cache_size = cache_mb << 20;
    jit_ = std::make_unique<Dynarmic::A64::Jit>(c);
    jit_ptr_.store(jit_.get(), std::memory_order_seq_cst);
    factory_.jit_cores_.fetch_add(1, std::memory_order_relaxed);
    g_jit_threads.fetch_add(1, std::memory_order_relaxed);
    static std::atomic<int> logged{0};
    if (logged++ < 20)
        LOG_INFO("Recomp", "JIT needed at {:#x} (thread slot {}): no native code there", st_.pc, slot_);
}

// Runs the JIT from st_ until it reaches native code again (or a halt). single_step: one instruction (debugger).
void DynarmicCore::RunJit(bool single_step) {
    // Every pc the JIT has to run is code the recompiler has not seen: record each once, in the generator's profile
    // format ("<pc hex> <count>"), so the next library build covers it (copy recomp_missing.txt into
    // build/recomp_work as prof<N>.txt).
    {
        static std::mutex m;
        static std::unordered_set<u64> seen;
        std::lock_guard lk(m);
        if (seen.insert(st_.pc).second) {
            if (FILE* f = std::fopen("recomp_missing.txt", "a")) {
                std::fprintf(f, "%llx 1\n", static_cast<unsigned long long>(st_.pc));
                std::fclose(f);
            }
        }
    }
    EnsureJit();
    auto regs = jit_->GetRegisters();
    std::memcpy(regs.data(), st_.x, sizeof(st_.x));
    jit_->SetRegisters(regs);
    auto vecs = jit_->GetVectors();
    std::memcpy(vecs.data(), st_.v, sizeof(st_.v));
    jit_->SetVectors(vecs);
    jit_->SetSP(st_.sp);
    jit_->SetPC(st_.pc);
    jit_->SetPstate(pstate_rest_ | st_.nzcv);
    jit_->SetFpcr(st_.fpcr);
    jit_->SetFpsr(st_.fpsr);
    tpidr_el0_ = st_.tpidr;
    tpidrro_el0_ = st_.tpidrro;
    in_jit_ = true;
    std::atomic_thread_fence(std::memory_order_seq_cst); // see RequestHalt
    if (single_step)
        jit_->Step();
    else if (!native_halt_)
        jit_->Run();
    in_jit_ = false;
    for (auto r : {kHaltStop, kHaltInvalidate, kHaltCallback, kHaltNative})
        jit_->ClearHalt(r);
    regs = jit_->GetRegisters();
    std::memcpy(st_.x, regs.data(), sizeof(st_.x));
    vecs = jit_->GetVectors();
    std::memcpy(st_.v, vecs.data(), sizeof(st_.v));
    st_.sp = jit_->GetSP();
    st_.pc = jit_->GetPC();
    const u32 pstate = jit_->GetPstate();
    st_.nzcv = pstate & 0xF0000000u;
    pstate_rest_ = pstate & 0x0FFFFFFFu;
    st_.fpcr = jit_->GetFpcr();
    st_.fpsr = jit_->GetFpsr();
    st_.tpidr = tpidr_el0_;
}

void DynarmicCore::MaybeReport() { factory_.MaybeReport(); }

recomp::Fn DynarmicCore::NativeAt(u64 pc) const {
    if (!factory_.TableReady()) [[unlikely]]
        factory_.EnsureNativeTable();
    return factory_.Lookup(pc);
}

void DynarmicCore::RunNativeChain(recomp::Fn fn) {
    auto& s = st_;
    const u64 pc = s.pc;
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
    host_mxcsr_ = _mm_getcsr();
    guest_mxcsr_ = GuestMxcsr(s.fpcr);
    if (guest_mxcsr_ != host_mxcsr_)
        _mm_setcsr(guest_mxcsr_);
    // Chain native functions (tail jumps, returns into other entries) until the guest leaves recompiled code.
    for (;;) {
        const u64 before = s.icount, at = s.pc;
        fn(s);
        // No progress (stack floor, a function that refused to run): the JIT has to run this pc. Host-implemented
        // C library routines return without counting instructions, but they do move the pc.
        if (s.icount == before && s.pc == at) {
            if (native_halt_)
                break; // a halt request, not a failure
            static std::atomic<int> reports{0};
            if (reports++ < 10)
                LOG_WARN("Recomp", "no progress at {:#x} (chain from {:#x}, stack {:#x}, floor {:#x})", at, pc,
                         reinterpret_cast<u64>(_AddressOfReturnAddress()), s.stack_limit);
            // Never enter native code here again, or Run() and the JIT would hand this pc back and forth forever.
            factory_.DisableNative(at);
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
    instr_window_.fetch_add(s.icount - icount0, std::memory_order_relaxed);
    if (!host_tid_)
        host_tid_ = GetCurrentThreadId();
}

void DynarmicCore::AddTicks(u64 ticks) {
    factory_.jit_instr_.fetch_add(ticks, std::memory_order_relaxed);
    factory_.MaybeReport();
}

} // namespace

std::atomic<float> g_native_percent{-1.0f};
std::atomic<u32> g_jit_threads{0};

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
void SetFpcr(State& s, u32 v) { CoreOf(s).NativeSetFpcr(v); }
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
namespace {
u8* GuestPtr(State& s, u64 a) { return s.mem + a; }
const char* GuestStr(State& s, u64 a) { return reinterpret_cast<const char*>(s.mem + a); }
// A range outside the guest's address space would fault on the Switch; on the host it would scribble over our own
// memory, so such calls do nothing (logged once).
bool GuestRange(u64 a, u64 n, const char* what) {
    if (a < core::layout::kSpaceEnd && n <= core::layout::kSpaceEnd - a)
        return true;
    static std::atomic<int> logged{0};
    if (logged++ < 8)
        LOG_WARN("Recomp", "{}: guest range {:#x}+{:#x} outside the address space, skipped", what, a, n);
    return false;
}
} // namespace
// Overlapping memcpy is undefined; memmove also covers callers that rely on the guest's forward copy.
void Libc_memcpy(State& s) {
    if (GuestRange(s.x[0], s.x[2], "memcpy") && GuestRange(s.x[1], s.x[2], "memcpy"))
        std::memmove(GuestPtr(s, s.x[0]), GuestPtr(s, s.x[1]), s.x[2]);
}
void Libc_memmove(State& s) {
    if (GuestRange(s.x[0], s.x[2], "memmove") && GuestRange(s.x[1], s.x[2], "memmove"))
        std::memmove(GuestPtr(s, s.x[0]), GuestPtr(s, s.x[1]), s.x[2]);
}
void Libc_memset(State& s) {
    if (GuestRange(s.x[0], s.x[2], "memset"))
        std::memset(GuestPtr(s, s.x[0]), int(u8(s.x[1])), s.x[2]);
}
void Libc_memcmp(State& s) {
    const u8 *a = GuestPtr(s, s.x[0]), *b = GuestPtr(s, s.x[1]);
    s.x[0] = 0;
    for (u64 i = 0, n = s.x[2]; i < n; i++)
        if (a[i] != b[i]) {
            s.x[0] = u64(s64(int(a[i]) - int(b[i])));
            return;
        }
}
void Libc_strlen(State& s) { s.x[0] = std::strlen(GuestStr(s, s.x[0])); }
void Libc_strcmp(State& s) {
    const u8 *a = GuestPtr(s, s.x[0]), *b = GuestPtr(s, s.x[1]);
    while (*a && *a == *b)
        a++, b++;
    s.x[0] = u64(s64(int(*a) - int(*b)));
}
void Libc_strncmp(State& s) {
    const u8 *a = GuestPtr(s, s.x[0]), *b = GuestPtr(s, s.x[1]);
    u64 n = s.x[2];
    while (n && *a && *a == *b)
        a++, b++, n--;
    s.x[0] = n ? u64(s64(int(*a) - int(*b))) : 0;
}
void Libc_memchr(State& s) {
    const void* p = std::memchr(GuestPtr(s, s.x[0]), int(u8(s.x[1])), s.x[2]);
    s.x[0] = p ? s.x[0] + u64(static_cast<const u8*>(p) - GuestPtr(s, s.x[0])) : 0;
}
void Libc_strchr(State& s) {
    const char* p = std::strchr(GuestStr(s, s.x[0]), char(s.x[1]));
    s.x[0] = p ? s.x[0] + u64(p - GuestStr(s, s.x[0])) : 0;
}
void Dispatch(State& s) {
    auto& f = CoreOf(s).Factory();
    if (f.census_) [[unlikely]]
        f.Census(s.pc);
    // While the game resolves its NVN entry points, nvnDeviceGetProcAddress(device, "nvnFoo") calls arrive here:
    // remember which driver function each name maps to (host NVN implementations replace them).
    if (f.capture_names_) [[unlikely]] {
        const u64 str = s.x[1];
        if (f.GuestValid(str, 64) && Rd<u8>(s, str) == 'n' &&
            Rd<u8>(s, str + 1) == 'v' && Rd<u8>(s, str + 2) == 'n') {
            char name[64]{};
            for (int i = 0; i < 63 && (name[i] = char(Rd<u8>(s, str + i))); i++)
                ;
            if (auto fn = f.Lookup(s.pc)) {
                const u64 pc = s.pc, device = s.x[0];
                fn(s);
                if (f.OnProcAddress(pc, s.x[0], name)) // all names seen: resolve the host ones never asked for
                    hle::nvn::ResolveMissing(s, fn, pc, device,
                                             [&](const char* n, u64 ptr) { f.OverrideWithHost(ptr, n); });
                return;
            }
        }
    }
    if (auto fn = f.Lookup(s.pc))
        fn(s);
}
} // namespace recomp
