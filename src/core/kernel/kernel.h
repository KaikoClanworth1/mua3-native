#pragma once
#include <atomic>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>

#include "common/common.h"
#include "core/cpu/cpu.h"
#include "core/memory.h"

namespace hle {
class ServiceManager;
class SessionObject;
}

namespace kernel {

using Handle = u32;
using Result = u32;
constexpr Handle kCurrentThread = 0xFFFF8000;
constexpr Handle kCurrentProcess = 0xFFFF8001;

namespace rc {
constexpr Result Success = 0;
constexpr Result InvalidSize = 0xCA01;
constexpr Result InvalidAddress = 0xCC01;
constexpr Result OutOfMemory = 0xD001;
constexpr Result OutOfHandles = 0xD201;
constexpr Result InvalidCurrentMemory = 0xD401;
constexpr Result InvalidMemoryRange = 0xDC01;
constexpr Result InvalidPriority = 0xE001;
constexpr Result InvalidCoreId = 0xE201;
constexpr Result InvalidHandle = 0xE401;
constexpr Result InvalidPointer = 0xE601;
constexpr Result InvalidCombination = 0xE801;
constexpr Result TimedOut = 0xEA01;
constexpr Result Cancelled = 0xEC01;
constexpr Result OutOfRange = 0xEE01;
constexpr Result InvalidEnumValue = 0xF001;
constexpr Result NotFound = 0xF201;
constexpr Result SessionClosed = 0xF601;
constexpr Result InvalidState = 0xFA01;
constexpr Result NotImplemented = 0x10A01; // generic stub failure
} // namespace rc

enum class ObjType { Thread, Event, Session, SharedMemory, TransferMemory, Process };

class Thread;

class KObject : public std::enable_shared_from_this<KObject> {
public:
    virtual ~KObject() = default;
    virtual ObjType Type() const = 0;
    virtual bool IsSignaled() const { return false; }
    std::string name;
    std::vector<Thread*> sync_waiters; // guarded by Kernel::lock
};
using KObjectPtr = std::shared_ptr<KObject>;

class Event final : public KObject {
public:
    explicit Event(std::string n = {}) { name = std::move(n); }
    ObjType Type() const override { return ObjType::Event; }
    bool IsSignaled() const override { return signaled; }
    bool signaled = false;
};

class SharedMemory final : public KObject {
public:
    SharedMemory(u64 size, std::string n) : backing(size, 0), size(size) { name = std::move(n); }
    ObjType Type() const override { return ObjType::SharedMemory; }
    // Host view. Once mapped by the guest this points into guest memory.
    u8* Data() { return mapped_ptr ? mapped_ptr : backing.data(); }
    std::vector<u8> backing;
    u64 size;
    u64 mapped_addr = 0;
    u8* mapped_ptr = nullptr;
};

class TransferMemory final : public KObject {
public:
    ObjType Type() const override { return ObjType::TransferMemory; }
    u64 addr = 0, size = 0;
    u32 perm = 0;
};

class Process final : public KObject {
public:
    ObjType Type() const override { return ObjType::Process; }
};

class Thread final : public KObject {
public:
    ObjType Type() const override { return ObjType::Thread; }
    bool IsSignaled() const override { return state == State::Exited; }

    enum class State { Created, Running, Exited };
    State state = State::Created;
    u64 id = 0;
    Handle handle = 0; // in the process handle table
    u64 entry = 0, arg = 0, stack_top = 0;
    s32 priority = 44;
    s32 core = 0;
    u64 affinity = 1;
    u64 tls = 0;
    std::unique_ptr<cpu::Core> cpu;
    std::thread host;
    void* host_handle = nullptr;           // native handle (for debugging dumps)
    std::atomic<bool> dump_ack{false};

    // Waiting (guarded by Kernel::lock)
    std::condition_variable cv;
    bool wakeup = false;
    Result wait_result = 0;
    s32 signaled_index = -1;
    bool cancel_pending = false;
    bool in_sync_wait = false;
    // Mutex / condvar arbitration
    u64 mutex_addr = 0;     // address we wait to acquire
    Handle mutex_tag = 0;   // our tag (requester handle)
    u64 cv_key = 0;         // condition variable key we wait on
    Thread* lock_owner = nullptr;
    // Address arbiter
    u64 arbiter_addr = 0;
    std::atomic<u32> current_svc{0}; // SVC being serviced (0 = running guest code)
};
using ThreadPtr = std::shared_ptr<Thread>;

class Kernel;

class HandleTable {
public:
    Handle Add(KObjectPtr obj);
    KObjectPtr Get(Handle h) const;
    template <typename T>
    std::shared_ptr<T> GetAs(Handle h) const {
        auto o = Get(h);
        return o ? std::dynamic_pointer_cast<T>(o) : nullptr;
    }
    bool Close(Handle h);
    size_t Count() const;

private:
    mutable std::mutex mutex_;
    std::unordered_map<Handle, KObjectPtr> table_;
    u32 next_ = 1;
};

// Per-host-thread pointer to the running guest thread.
Thread* CurrentThread();

class Kernel {
public:
    Kernel(core::Memory& mem);
    ~Kernel();

    core::Memory& Mem() { return mem_; }
    HandleTable& Handles() { return handles_; }
    hle::ServiceManager& Services() { return *services_; }
    cpu::CoreFactory& Cpus() { return *cpus_; }
    std::mutex lock; // global kernel lock

    // Threads
    ThreadPtr CreateThread(u64 entry, u64 arg, u64 stack_top, s32 prio, s32 core, std::string name);
    void StartThread(const ThreadPtr& t);
    ThreadPtr GetThread(Handle h);
    void ExitCurrentThread();
    [[noreturn]] void ExitProcess(int code = 0);
    bool Exiting() const { return exiting_; }

    // Sync primitives
    void SignalObject(KObject& obj); // must hold lock; wakes WaitSynchronization waiters
    void SignalEvent(Event& e);
    // Waits on `t->cv` until woken or timed out (timeout_ns < 0 = infinite). Must hold `lk`.
    Result WaitCurrent(std::unique_lock<std::mutex>& lk, Thread* t, s64 timeout_ns);
    void Wake(Thread* t, Result r);

    // Called from CPU
    void HandleSvc(Thread* t, u32 imm);
    void HandleException(Thread* t, u64 pc, int kind);
    u64 GetTicks() const;

    // HLE function hooks: replaces the guest function at `addr` with `fn`.
    using HookFn = std::function<void(Thread*)>;
    void InstallHook(u64 addr, std::string name, HookFn fn);

    // Allocate a 0x200-byte TLS slot.
    u64 AllocTls();
    void DumpThreadState(Thread* t, bool error = true);
    // Logs where every guest thread is (running threads are briefly interrupted).
    void DumpAllThreads();

    u64 program_id = 0x010060700AC50000ull;
    u64 random_entropy[4]{};

    // Per-thread recursion guard for hooks and IPC
    std::vector<std::pair<std::string, HookFn>> hooks;

private:
    void ThreadMain(ThreadPtr t);
    void SvcDispatch(Thread* t, u32 id);

    core::Memory& mem_;
    HandleTable handles_;
    std::unique_ptr<cpu::CoreFactory> cpus_;
    std::unique_ptr<hle::ServiceManager> services_;
    std::vector<ThreadPtr> threads_;
    std::mutex threads_mutex_;
    std::atomic<u64> next_thread_id_{1};
    std::atomic<bool> exiting_{false};
    std::vector<u64> tls_free_;
    std::mutex tls_mutex_;
    s64 qpc_freq_ = 0, qpc_start_ = 0;

    friend class Svc;
public:
    // Mutex/condvar/arbiter wait lists (guarded by lock)
    std::vector<Thread*> cv_waiters;
    std::vector<Thread*> arb_waiters;
    std::vector<Thread*> lock_waiters;
};

Kernel& K(); // global instance

} // namespace kernel
