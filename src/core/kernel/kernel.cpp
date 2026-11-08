#include "core/kernel/kernel.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <random>
#include <windows.h>
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")

#include "core/hle/service.h"
#include "core/system.h"

namespace kernel {

static Kernel* g_kernel = nullptr;
static thread_local Thread* t_current = nullptr;

Kernel& K() { return *g_kernel; }
Thread* CurrentThread() { return t_current; }

// ---------------------------------------------------------------- HandleTable
Handle HandleTable::Add(KObjectPtr obj) {
    std::lock_guard lk(mutex_);
    // Any non-zero value below the mutex "has waiters" bit (0x40000000) that isn't a pseudo-handle works.
    Handle h;
    do {
        h = 0x10000 + (next_++ & 0x3FFFFFF);
    } while (table_.count(h));
    table_[h] = std::move(obj);
    return h;
}

KObjectPtr HandleTable::Get(Handle h) const {
    std::lock_guard lk(mutex_);
    auto it = table_.find(h);
    return it == table_.end() ? nullptr : it->second;
}

bool HandleTable::Close(Handle h) {
    std::lock_guard lk(mutex_);
    return table_.erase(h) != 0;
}

size_t HandleTable::Count() const {
    std::lock_guard lk(mutex_);
    return table_.size();
}

// ---------------------------------------------------------------- Kernel
Kernel::Kernel(core::Memory& mem) : mem_(mem) {
    g_kernel = this;
    cpus_ = cpu::CreateDynarmicFactory(mem, 256);
    services_ = std::make_unique<hle::ServiceManager>(*this);
    StartFreezeWatch();
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    qpc_freq_ = f.QuadPart;
    qpc_start_ = c.QuadPart;
    std::random_device rd;
    for (auto& e : random_entropy)
        e = (u64(rd()) << 32) | rd();
}

Kernel::~Kernel() = default;

u64 Kernel::GetTicks() const {
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    const s64 d = c.QuadPart - qpc_start_;
    // 19.2 MHz
    return u64((d / qpc_freq_) * 19200000 + (d % qpc_freq_) * 19200000 / qpc_freq_);
}

u64 Kernel::AllocTls() {
    std::lock_guard lk(tls_mutex_);
    if (tls_free_.empty()) {
        const u64 page = mem_.FindFree(core::layout::kKernelMapBase, 0x1000000000ull, 0x1000);
        mem_.Map(page, 0x1000, core::MS_ThreadLocal, core::MP_RW);
        for (int i = 7; i >= 0; i--)
            tls_free_.push_back(page + i * 0x200);
    }
    const u64 a = tls_free_.back();
    tls_free_.pop_back();
    std::memset(mem_.Ptr(a), 0, 0x200);
    return a;
}

ThreadPtr Kernel::CreateThread(u64 entry, u64 arg, u64 stack_top, s32 prio, s32 core, std::string name) {
    auto t = std::make_shared<Thread>();
    t->name = std::move(name);
    t->id = next_thread_id_++;
    t->entry = entry;
    t->arg = arg;
    t->stack_top = stack_top;
    t->priority = prio;
    t->core = core < 0 ? 0 : core;
    t->affinity = 1ull << t->core;
    t->tls = AllocTls();
    t->handle = handles_.Add(t);

    Thread* raw = t.get();
    cpu::CpuCallbacks cb;
    cb.svc = [this, raw](u32 imm) { HandleSvc(raw, imm); };
    cb.exception = [this, raw](u64 pc, int kind) { HandleException(raw, pc, kind); };
    cb.cntpct = [this] { return GetTicks(); };
    t->cpu = cpus_->Create(std::move(cb), 0);
    t->cpu->SetPC(entry);
    t->cpu->SetSP(stack_top);
    t->cpu->SetReg(0, arg);
    t->cpu->SetTpidrroEl0(t->tls);
    t->cpu->SetFpcr(0);
    {
        std::lock_guard lk(threads_mutex_);
        threads_.push_back(t);
    }
    LOG_DEBUG("Kernel", "CreateThread id={} entry={:#x} arg={:#x} sp={:#x} prio={} core={} handle={:#x}", t->id,
              entry, arg, stack_top, prio, core, t->handle);
    return t;
}

void Kernel::StartThread(const ThreadPtr& t) {
    t->state = Thread::State::Running;
    // Recompiled guest code recurses on the host stack, with frames larger than the guest's: a 32 MB reservation
    // (committed only as it is used) keeps deep guest call chains native instead of hitting the stack floor.
    struct Start {
        Kernel* kernel;
        ThreadPtr thread;
    };
    auto* start = new Start{this, t};
    const HANDLE h = ::CreateThread(
        nullptr, 32u << 20,
        [](LPVOID p) -> DWORD {
            std::unique_ptr<Start> s(static_cast<Start*>(p));
            s->kernel->ThreadMain(s->thread);
            return 0;
        },
        start, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (!h) {
        delete start;
        PANIC("CreateThread failed for guest thread {} ({})", t->id, GetLastError());
    }
    CloseHandle(h);
}

ThreadPtr Kernel::GetThread(Handle h) {
    if (h == kCurrentThread)
        return t_current ? std::static_pointer_cast<Thread>(t_current->shared_from_this()) : nullptr;
    return handles_.GetAs<Thread>(h);
}

static int MapPriority(s32 guest) {
    // Guest: 0 (highest) .. 63 (lowest); games mostly use 16..59.
    if (guest <= 20)
        return THREAD_PRIORITY_HIGHEST;
    if (guest <= 30)
        return THREAD_PRIORITY_ABOVE_NORMAL;
    if (guest <= 44)
        return THREAD_PRIORITY_NORMAL;
    if (guest <= 54)
        return THREAD_PRIORITY_BELOW_NORMAL;
    return THREAD_PRIORITY_LOWEST;
}

void Kernel::ThreadMain(ThreadPtr t) {
    t_current = t.get();
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &t->host_handle, 0, FALSE,
                    DUPLICATE_SAME_ACCESS);
    {
        std::wstring wname(t->name.begin(), t->name.end());
        SetThreadDescription(GetCurrentThread(), (L"guest:" + wname).c_str());
        SetThreadPriority(GetCurrentThread(), MapPriority(t->priority));
    }
    LOG_DEBUG("Kernel", "Thread {} ({}) started at {:#x}", t->id, t->name, t->entry);
    t->cpu->Run();
    // Exited via svcExitThread (or the CPU was halted for process exit).
    LOG_DEBUG("Kernel", "Thread {} ({}) exited at {}", t->id, t->name, core::Sys().Symbolize(t->cpu->PC()));
    {
        std::lock_guard lk(lock);
        t->state = Thread::State::Exited;
        SignalObject(*t);
    }
    {
        std::lock_guard lk(threads_mutex_);
        threads_.erase(std::remove(threads_.begin(), threads_.end(), t), threads_.end());
    }
    // Free the JIT (and its exclusive-monitor slot); the Thread object lives on while handles reference it.
    t->cpu.reset();
    {
        std::lock_guard lk(tls_mutex_);
        tls_free_.push_back(t->tls);
    }
    if (t->host_handle)
        CloseHandle((HANDLE)t->host_handle);
    t->host_handle = nullptr;
}

void Kernel::ExitCurrentThread() {
    Thread* t = t_current;
    t->cpu->Halt();
}

void Kernel::ExitProcess(int code) {
    exiting_ = true;
    LOG_INFO("Kernel", "Process exit requested ({})", code);
    std::fflush(stderr);
    TerminateProcess(GetCurrentProcess(), code);
    std::abort();
}

void Kernel::SignalObject(KObject& obj) {
    auto waiters = obj.sync_waiters;
    for (Thread* w : waiters) {
        if (!w->in_sync_wait)
            continue;
        Wake(w, rc::Success);
    }
}

void Kernel::SignalEvent(Event& e) {
    std::lock_guard lk(lock);
    e.signaled = true;
    SignalObject(e);
}

void Kernel::Wake(Thread* t, Result r) {
    t->wait_result = r;
    t->wakeup = true;
    t->cv.notify_one();
}

Result Kernel::WaitCurrent(std::unique_lock<std::mutex>& lk, Thread* t, s64 timeout_ns) {
    t->wakeup = false;
    t->wait_result = rc::TimedOut;
    if (timeout_ns < 0) {
        t->cv.wait(lk, [&] { return t->wakeup; });
    } else {
        t->cv.wait_for(lk, std::chrono::nanoseconds(timeout_ns), [&] { return t->wakeup; });
    }
    return t->wakeup ? t->wait_result : rc::TimedOut;
}

void Kernel::InstallHook(u64 addr, std::string name, HookFn fn) {
    const u32 idx = u32(hooks.size());
    hooks.emplace_back(std::move(name), std::move(fn));
    // svc #(0x1000 + idx); ret
    mem_.Write<u32>(addr, 0xD4000001u | ((0x1000u + idx) << 5));
    mem_.Write<u32>(addr + 4, 0xD65F03C0u);
    cpus_->InvalidateAll(addr, 8);
}

void Kernel::InstallTrace(u64 addr, std::string name, HookFn fn) {
    constexpr u64 kTrampolineSize = 0x10000, kSlot = 32;
    if (!trampolines_) {
        trampolines_ = mem_.FindFree(core::layout::kKernelMapBase, 0x1000000000ull, kTrampolineSize);
        mem_.Map(trampolines_, kTrampolineSize, core::MS_Normal, core::MP_RW); // not Code: rtld scans code regions for modules
    }
    if (trampoline_used_ + kSlot > kTrampolineSize)
        return;
    const u64 tramp = trampolines_ + trampoline_used_;
    trampoline_used_ += kSlot;
    // +0 original instruction, +4 ldr x16, [pc+12], +8 br x16, +16 return address (addr + 4)
    mem_.Write<u32>(tramp, mem_.Read<u32>(addr));
    mem_.Write<u32>(tramp + 4, 0x58000070u);
    mem_.Write<u32>(tramp + 8, 0xD61F0200u);
    mem_.Write<u32>(tramp + 12, 0xD503201Fu);
    mem_.Write<u64>(tramp + 16, addr + 4);
    const u32 idx = u32(hooks.size());
    hooks.emplace_back(std::move(name), [fn = std::move(fn), tramp](Thread* t) {
        fn(t);
        t->cpu->SetPC(tramp);
    });
    mem_.Write<u32>(addr, 0xD4000001u | ((0x1000u + idx) << 5));
    cpus_->InvalidateAll(addr, 4);
}

void Kernel::HandleSvc(Thread* t, u32 imm) {
    // Stall trace (MUA3_STALL_MS=<ms>): SVCs and HLE hooks that hold a guest thread longer than that, with their
    // arguments and caller, to find what a game freeze waits on.
    static const s64 stall_ns = [] {
        const char* e = std::getenv("MUA3_STALL_MS");
        return e ? s64(std::atoll(e)) * 1'000'000 : 0;
    }();
    const auto t0 = stall_ns ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    u64 a[4]{}, lr = 0;
    if (stall_ns) {
        for (int i = 0; i < 4; i++)
            a[i] = t->cpu->Reg(i);
        lr = t->cpu->Reg(30);
    }
    auto report = [&](const std::string& what) {
        const s64 ns = (std::chrono::steady_clock::now() - t0).count();
        if (ns >= stall_ns && imm != 0x0B) // SleepThread is deliberate
            LOG_WARN("Stall", "{} ({}) {} took {:.1f} ms: x0 {:#x} x1 {:#x} x2 {:#x} x3 {:#x} -> {:#x}, caller {}", t->id,
                     t->name, what, double(ns) / 1e6, a[0], a[1], a[2], a[3], t->cpu->Reg(0),
                     core::Sys().Symbolize(lr));
    };
    if (imm >= 0x1000) {
        const u32 idx = imm - 0x1000;
        if (idx < hooks.size()) {
            hooks[idx].second(t);
            if (stall_ns)
                report("hook " + hooks[idx].first);
            return;
        }
    }
    t->current_svc = imm;
    SvcDispatch(t, imm);
    t->current_svc = 0;
    if (stall_ns)
        report(fmt::format("svc {:#x}", imm));
}

void Kernel::DumpThreadState(Thread* t, bool error) {
    auto& c = *t->cpu;
    auto& sys = core::Sys();
    std::string s = fmt::format("Thread {} ({}) prio {} pc={} lr={} sp={:#x}\n", t->id, t->name, t->priority,
                                sys.Symbolize(c.PC()), sys.Symbolize(c.Reg(30)), c.SP());
    if (error) {
        for (int i = 0; i < 31; i += 4) {
            for (int j = i; j < std::min(i + 4, 31); j++)
                s += fmt::format("  x{:<2}={:016x}", j, c.Reg(j));
            s += "\n";
        }
    }
    // Frame-pointer backtrace
    u64 fp = c.Reg(29);
    s += "  backtrace:\n";
    for (int i = 0; i < 24 && fp && mem_.IsValid(fp, 16); i++) {
        const u64 lr = mem_.Read<u64>(fp + 8);
        s += fmt::format("    {}\n", sys.Symbolize(lr));
        fp = mem_.Read<u64>(fp);
    }
    if (error)
        LOG_ERROR("Kernel", "{}", s);
    else
        LOG_INFO("Kernel", "{}", s);
}

// Samples the host thread backing a guest thread (for hangs outside guest code).
static void DumpHostStack(Thread* t, const char* why = "not in guest code", int depth = 20) {
    static bool sym_init = [] {
        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
        char exe[MAX_PATH]{}; // the pdb sits next to the exe, which is not the working directory
        GetModuleFileNameA(nullptr, exe, MAX_PATH);
        std::string dir(exe);
        dir = dir.substr(0, dir.find_last_of("\\/"));
        return SymInitialize(GetCurrentProcess(), dir.c_str(), TRUE) != FALSE;
    }();
    HANDLE h = (HANDLE)t->host_handle;
    if (!h || SuspendThread(h) == DWORD(-1)) {
        LOG_INFO("Kernel", "Thread {} ({}) not responding (no host handle)", t->id, t->name);
        return;
    }
    CONTEXT ctx{};
    ctx.ContextFlags = CONTEXT_FULL;
    GetThreadContext(h, &ctx);
    STACKFRAME64 frame{};
    frame.AddrPC.Offset = ctx.Rip;
    frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrFrame.Offset = ctx.Rbp;
    frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Offset = ctx.Rsp;
    frame.AddrStack.Mode = AddrModeFlat;
    // Walk while suspended, name the frames after resuming: the thread may hold the heap or loader lock.
    u64 pcs[64];
    int n = 0;
    while (n < std::min(depth, 64) &&
           StackWalk64(IMAGE_FILE_MACHINE_AMD64, GetCurrentProcess(), h, &frame, &ctx, nullptr, SymFunctionTableAccess64,
                       SymGetModuleBase64, nullptr) && frame.AddrPC.Offset)
        pcs[n++] = frame.AddrPC.Offset;
    ResumeThread(h);
    std::string s = fmt::format("Thread {} ({}) {}; host stack:\n", t->id, t->name, why);
    for (int i = 0; i < n; i++) {
        alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 256]{};
        auto* sym = reinterpret_cast<SYMBOL_INFO*>(buf);
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 255;
        DWORD64 disp = 0;
        HMODULE mod = nullptr;
        char name[MAX_PATH] = "?";
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCSTR>(pcs[i]), &mod))
            GetModuleFileNameA(mod, name, MAX_PATH);
        const char* base = std::max(std::strrchr(name, '\\'),std::strrchr(name, '/'));
        base = base ? base + 1 : name;
        if (sym_init && SymFromAddr(GetCurrentProcess(), pcs[i], &disp, sym))
            s += fmt::format("    {}!{}+{:#x}\n", base, sym->Name, disp);
        else
            s += fmt::format("    {}+{:#x}\n", base, mod ? pcs[i] - u64(mod) : pcs[i]);
    }
    LOG_INFO("Kernel", "{}", s);
}

// Freeze watchdog (MUA3_FREEZE_MS=<ms>): when no frame was presented for that long, the host stacks of the guest
// threads that are running (not parked in an SVC) say what holds the game up. A few samples per freeze.
void Kernel::StartFreezeWatch() {
    const char* e = std::getenv("MUA3_FREEZE_MS");
    if (!e)
        return;
    const s64 limit = s64(std::atoll(e)) * 1'000'000;
    std::thread([this, limit] {
        SetThreadDescription(GetCurrentThread(), L"FreezeWatch");
        long long reported = 0;
        int samples = 0;
        for (;;) {
            Sleep(50);
            const long long last = common::g_last_present_ns.load(std::memory_order_relaxed);
            const long long now = std::chrono::steady_clock::now().time_since_epoch().count();
            if (!last || now - last < limit)
                continue;
            if (last != reported)
                reported = last, samples = 0;
            if (samples >= 8)
                continue;
            samples++;
            LOG_WARN("Freeze", "no frame for {:.0f} ms (sample {})", double(now - last) / 1e6, samples);
            std::vector<ThreadPtr> ts;
            {
                std::lock_guard lk(threads_mutex_);
                ts = threads_;
            }
            for (auto& t : ts)
                if (t->state == Thread::State::Running && !t->current_svc)
                    DumpHostStack(t.get(), "running", 48);
            Sleep(100);
        }
    }).detach();
}

void Kernel::DumpAllThreads(bool host_stacks) {
    std::vector<ThreadPtr> ts;
    {
        std::lock_guard lk(threads_mutex_);
        ts = threads_;
    }
    if (lock.try_lock())
        lock.unlock();
    else
        LOG_INFO("Kernel", "(kernel lock is currently held)");
    for (auto& t : ts) {
        if (t->state != Thread::State::Running) {
            LOG_INFO("Kernel", "Thread {} ({}) state {}", t->id, t->name, int(t->state));
            continue;
        }
        const u32 svc = t->current_svc;
        if (svc && host_stacks) {
            DumpHostStack(t.get());
            continue;
        }
        if (svc) {
            // The thread is parked in an SVC callback, so its guest registers are stable enough to walk.
            std::string bt;
            u64 fp = t->cpu->Reg(29);
            for (int i = 0; i < 10 && fp && mem_.IsValid(fp, 16); i++) {
                bt += " <- " + core::Sys().Symbolize(mem_.Read<u64>(fp + 8));
                fp = mem_.Read<u64>(fp);
            }
            LOG_INFO("Kernel", "Thread {} ({}) blocked in SVC {:#x}{}, lr={}{}", t->id, t->name, svc,
                     svc == 0x21 ? " (" + t->current_ipc + ")" : std::string(), core::Sys().Symbolize(t->cpu->Reg(30)), bt);
            continue;
        }
        Thread* raw = t.get();
        t->dump_ack = false;
        t->cpu->RunOnCore([this, raw] {
            raw->dump_ack = true;
            DumpThreadState(raw, false);
        });
        for (int i = 0; i < 30 && !t->dump_ack; i++)
            Sleep(10);
        if (!t->dump_ack)
            DumpHostStack(t.get());
    }
}

void Kernel::DumpThreadRegs(u32 thread_id) {
    std::lock_guard lk(threads_mutex_);
    for (auto& t : threads_)
        if (t->id == thread_id && t->cpu)
            DumpThreadState(t.get(), true);
}

void Kernel::HandleException(Thread* t, u64 pc, int kind) {
    LOG_ERROR("Kernel", "Guest exception {} at {:#x} (insn {:08x})", kind, pc,
              mem_.IsValid(pc, 4) ? mem_.Read<u32>(pc) : 0);
    DumpThreadState(t);
    PANIC("Unhandled guest exception");
}

} // namespace kernel
