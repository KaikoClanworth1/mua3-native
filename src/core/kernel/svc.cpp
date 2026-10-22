#include <algorithm>
#include <atomic>
#include <thread>

#include <windows.h>

#include "core/hle/service.h"
#include "core/kernel/kernel.h"

namespace kernel {

using namespace core;

namespace {
constexpr u32 kHasWaiters = 0x40000000;

u32 AtomicLoad(Memory& m, u64 a) { return std::atomic_ref<u32>(*m.Ptr<u32>(a)).load(); }
void AtomicStore(Memory& m, u64 a, u32 v) { std::atomic_ref<u32>(*m.Ptr<u32>(a)).store(v); }
bool AtomicCas(Memory& m, u64 a, u32& expected, u32 v) {
    return std::atomic_ref<u32>(*m.Ptr<u32>(a)).compare_exchange_strong(expected, v);
}

template <typename V>
void Erase(V& v, Thread* t) {
    v.erase(std::remove(v.begin(), v.end(), t), v.end());
}

Thread* HighestPriority(const std::vector<Thread*>& v, auto pred) {
    Thread* best = nullptr;
    for (Thread* t : v)
        if (pred(t) && (!best || t->priority < best->priority))
            best = t;
    return best;
}

void PreciseSleep(s64 ns) {
    if (ns <= 0)
        return;
    static thread_local HANDLE timer =
        CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (timer) {
        LARGE_INTEGER due;
        due.QuadPart = -(ns / 100);
        if (due.QuadPart == 0)
            due.QuadPart = -1;
        SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
        WaitForSingleObject(timer, INFINITE);
    } else {
        std::this_thread::sleep_for(std::chrono::nanoseconds(ns));
    }
}
} // namespace

void Kernel::SvcDispatch(Thread* t, u32 id) {
    cpu::Core& c = *t->cpu;
    auto R = [&](int i) { return c.Reg(i); };
    auto W = [&](int i, u64 v) { c.SetReg(i, v); };
    auto Ret = [&](Result r) { W(0, r); };
    Memory& m = mem_;

    switch (id) {
    case 0x01: { // SetHeapSize
        const u64 size = R(1);
        if (size & 0x1FFFFF) {
            Ret(rc::InvalidSize);
            break;
        }
        if (!m.SetHeapSize(size)) {
            Ret(rc::OutOfMemory);
            break;
        }
        LOG_DEBUG("SVC", "SetHeapSize({:#x})", size);
        W(1, layout::kHeapBase);
        Ret(0);
        break;
    }
    case 0x02: // SetMemoryPermission
        m.SetPerm(R(0), R(1), u32(R(2)));
        Ret(0);
        break;
    case 0x03: // SetMemoryAttribute
        m.SetAttr(R(0), R(1), u32(R(2)), u32(R(3)));
        Ret(0);
        break;
    case 0x04: { // MapMemory(dst, src, size)
        const u64 dst = R(0), src = R(1), size = R(2);
        if (!m.Map(dst, size, MS_Stack, MP_RW)) {
            Ret(rc::InvalidMemoryRange);
            break;
        }
        std::memcpy(m.Ptr(dst), m.Ptr(src), size);
        m.SetPerm(src, size, MP_None);
        m.SetAttr(src, size, 1, 1); // locked
        LOG_DEBUG("SVC", "MapMemory({:#x} <- {:#x}, {:#x})", dst, src, size);
        Ret(0);
        break;
    }
    case 0x05: { // UnmapMemory(dst, src, size)
        const u64 dst = R(0), src = R(1), size = R(2);
        std::memcpy(m.Ptr(src), m.Ptr(dst), size);
        m.Unmap(dst, size);
        m.SetPerm(src, size, MP_RW);
        m.SetAttr(src, size, 1, 0);
        Ret(0);
        break;
    }
    case 0x06: { // QueryMemory(out info, out page info, addr)
        MemoryInfo mi = m.Query(R(2));
        mi.state &= 0xFF; // the SVC reports MemoryType only, not the kernel state flags
        m.Write(R(0), mi);
        LOG_TRACE("SVC", "QueryMemory({:#x}) -> {:#x}+{:#x} state {:#x} perm {}", R(2), mi.addr, mi.size, mi.state, mi.perm);
        W(1, 0);
        Ret(0);
        break;
    }
    case 0x07: // ExitProcess
        ExitProcess(0);
    case 0x08: { // CreateThread(_, entry, arg, stack_top, prio, core)
        s32 core = s32(R(5));
        if (core == -2)
            core = 0;
        auto nt = CreateThread(R(1), R(2), R(3), s32(R(4)), core, fmt::format("thread@{:x}", R(1)));
        W(1, nt->handle);
        Ret(0);
        break;
    }
    case 0x09: { // StartThread
        auto th = GetThread(Handle(R(0)));
        if (!th) {
            Ret(rc::InvalidHandle);
            break;
        }
        StartThread(th);
        Ret(0);
        break;
    }
    case 0x0A: // ExitThread
        ExitCurrentThread();
        break;
    case 0x0B: { // SleepThread
        const s64 ns = s64(R(0));
        if (ns <= 0)
            SwitchToThread();
        else
            PreciseSleep(ns);
        break;
    }
    case 0x0C: { // GetThreadPriority(_, handle)
        auto th = GetThread(Handle(R(1)));
        if (!th) {
            Ret(rc::InvalidHandle);
            break;
        }
        W(1, u32(th->priority));
        Ret(0);
        break;
    }
    case 0x0D: { // SetThreadPriority
        auto th = GetThread(Handle(R(0)));
        if (!th) {
            Ret(rc::InvalidHandle);
            break;
        }
        th->priority = s32(R(1));
        Ret(0);
        break;
    }
    case 0x0E: { // GetThreadCoreMask(_, _, handle)
        auto th = GetThread(Handle(R(2)));
        if (!th) {
            Ret(rc::InvalidHandle);
            break;
        }
        W(1, u32(th->core));
        W(2, th->affinity);
        Ret(0);
        break;
    }
    case 0x0F: { // SetThreadCoreMask(handle, core, mask)
        auto th = GetThread(Handle(R(0)));
        if (!th) {
            Ret(rc::InvalidHandle);
            break;
        }
        s32 core = s32(R(1));
        if (core >= 0)
            th->core = core;
        if (R(2))
            th->affinity = R(2);
        Ret(0);
        break;
    }
    case 0x10: // GetCurrentProcessorNumber
        W(0, u32(t->core));
        break;
    case 0x11: { // SignalEvent
        auto e = handles_.GetAs<Event>(Handle(R(0)));
        if (!e) {
            Ret(rc::InvalidHandle);
            break;
        }
        SignalEvent(*e);
        Ret(0);
        break;
    }
    case 0x12:   // ClearEvent
    case 0x17: { // ResetSignal
        auto e = handles_.GetAs<Event>(Handle(R(0)));
        if (!e) {
            Ret(id == 0x17 ? rc::Success : rc::InvalidHandle);
            break;
        }
        std::lock_guard lk(lock);
        const bool was = e->signaled;
        e->signaled = false;
        Ret(id == 0x17 && !was ? rc::InvalidState : rc::Success);
        break;
    }
    case 0x13: { // MapSharedMemory(handle, addr, size, perm)
        auto sm = handles_.GetAs<SharedMemory>(Handle(R(0)));
        if (!sm) {
            Ret(rc::InvalidHandle);
            break;
        }
        const u64 addr = R(1), size = R(2);
        if (!m.Map(addr, common::AlignUp(size, 0x1000), MS_Shared, u32(R(3)))) {
            Ret(rc::InvalidMemoryRange);
            break;
        }
        std::memcpy(m.Ptr(addr), sm->backing.data(), std::min<u64>(size, sm->backing.size()));
        sm->mapped_addr = addr;
        sm->mapped_ptr = m.Ptr(addr);
        LOG_DEBUG("SVC", "MapSharedMemory({}, {:#x}, {:#x})", sm->name, addr, size);
        Ret(0);
        break;
    }
    case 0x14: { // UnmapSharedMemory
        auto sm = handles_.GetAs<SharedMemory>(Handle(R(0)));
        if (sm && sm->mapped_ptr) {
            std::memcpy(sm->backing.data(), sm->mapped_ptr, sm->backing.size());
            sm->mapped_ptr = nullptr;
            sm->mapped_addr = 0;
        }
        m.Unmap(R(1), common::AlignUp(R(2), 0x1000));
        Ret(0);
        break;
    }
    case 0x15: { // CreateTransferMemory(_, addr, size, perm)
        auto tm = std::make_shared<TransferMemory>();
        tm->addr = R(1);
        tm->size = R(2);
        tm->perm = u32(R(3));
        m.SetAttr(tm->addr, tm->size, 1, 1);
        W(1, handles_.Add(tm));
        Ret(0);
        break;
    }
    case 0x16: { // CloseHandle
        const Handle h = Handle(R(0));
        if (auto tm = handles_.GetAs<TransferMemory>(h))
            m.SetAttr(tm->addr, tm->size, 1, 0);
        if (auto s = handles_.GetAs<hle::SessionObject>(h))
            s->OnClose();
        Ret(handles_.Close(h) || h == kCurrentThread || h == kCurrentProcess ? rc::Success : rc::InvalidHandle);
        break;
    }
    case 0x18: { // WaitSynchronization(_, handles, count, timeout)
        const u64 hp = R(1);
        const u32 count = u32(R(2));
        const s64 timeout = s64(R(3));
        if (count > 0x40) {
            Ret(rc::OutOfRange);
            break;
        }
        std::vector<KObjectPtr> objs;
        for (u32 i = 0; i < count; i++) {
            const Handle h = m.Read<u32>(hp + i * 4);
            KObjectPtr o = h == kCurrentThread ? t->shared_from_this() : handles_.Get(h);
            if (!o) {
                LOG_WARN("SVC", "WaitSynchronization: invalid handle {:#x}", h);
                Ret(rc::InvalidHandle);
                return;
            }
            objs.push_back(std::move(o));
        }
        std::unique_lock lk(lock);
        if (t->cancel_pending) {
            t->cancel_pending = false;
            Ret(rc::Cancelled);
            break;
        }
        auto find_signaled = [&]() -> s32 {
            for (u32 i = 0; i < count; i++)
                if (objs[i]->IsSignaled())
                    return s32(i);
            return -1;
        };
        s32 idx = find_signaled();
        if (idx >= 0) {
            W(1, u32(idx));
            Ret(0);
            break;
        }
        if (timeout == 0) {
            Ret(rc::TimedOut);
            break;
        }
        for (auto& o : objs)
            o->sync_waiters.push_back(t);
        t->in_sync_wait = true;
        Result r;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::nanoseconds(timeout < 0 ? 0 : timeout);
        for (;;) {
            s64 remaining = -1;
            if (timeout > 0)
                remaining = std::max<s64>(0, (deadline - std::chrono::steady_clock::now()).count());
            r = WaitCurrent(lk, t, remaining);
            if (r == rc::Cancelled || r == rc::TimedOut)
                break;
            idx = find_signaled();
            if (idx >= 0)
                break;
            // Spurious (signal was consumed by someone else); keep waiting.
        }
        t->in_sync_wait = false;
        for (auto& o : objs)
            Erase(o->sync_waiters, t);
        if (r == rc::Cancelled) {
            t->cancel_pending = false;
            Ret(rc::Cancelled);
        } else if (idx >= 0) {
            W(1, u32(idx));
            Ret(0);
        } else {
            Ret(rc::TimedOut);
        }
        break;
    }
    case 0x19: { // CancelSynchronization
        auto th = GetThread(Handle(R(0)));
        if (!th) {
            Ret(rc::InvalidHandle);
            break;
        }
        std::lock_guard lk(lock);
        if (th->in_sync_wait)
            Wake(th.get(), rc::Cancelled);
        else
            th->cancel_pending = true;
        Ret(0);
        break;
    }
    case 0x1A: { // ArbitrateLock(owner_handle, addr, requester)
        const Handle owner_h = Handle(R(0));
        const u64 addr = R(1);
        const Handle req = Handle(R(2));
        std::unique_lock lk(lock);
        const u32 v = AtomicLoad(m, addr);
        if (v != (owner_h | kHasWaiters)) {
            Ret(0);
            break;
        }
        auto owner = GetThread(owner_h);
        t->mutex_addr = addr;
        t->mutex_tag = req;
        t->lock_owner = owner.get();
        lock_waiters.push_back(t);
        const Result r = WaitCurrent(lk, t, -1);
        Erase(lock_waiters, t);
        t->lock_owner = nullptr;
        Ret(r);
        break;
    }
    case 0x1B: { // ArbitrateUnlock(addr)
        const u64 addr = R(0);
        std::lock_guard lk(lock);
        Thread* next = HighestPriority(lock_waiters, [&](Thread* w) { return w->mutex_addr == addr; });
        if (!next) {
            AtomicStore(m, addr, 0);
        } else {
            Erase(lock_waiters, next);
            const bool more = std::any_of(lock_waiters.begin(), lock_waiters.end(),
                                          [&](Thread* w) { return w->mutex_addr == addr; });
            AtomicStore(m, addr, next->mutex_tag | (more ? kHasWaiters : 0));
            for (Thread* w : lock_waiters)
                if (w->mutex_addr == addr)
                    w->lock_owner = next;
            next->mutex_addr = 0;
            Wake(next, 0);
        }
        Ret(0);
        break;
    }
    case 0x1C: { // WaitProcessWideKeyAtomic(mutex_addr, cv_key, tag, timeout)
        const u64 maddr = R(0), key = R(1);
        const Handle tag = Handle(R(2));
        const s64 timeout = s64(R(3));
        std::unique_lock lk(lock);
        // Release the mutex.
        {
            Thread* next = HighestPriority(lock_waiters, [&](Thread* w) { return w->mutex_addr == maddr; });
            if (!next) {
                AtomicStore(m, maddr, 0);
            } else {
                Erase(lock_waiters, next);
                const bool more = std::any_of(lock_waiters.begin(), lock_waiters.end(),
                                              [&](Thread* w) { return w->mutex_addr == maddr; });
                AtomicStore(m, maddr, next->mutex_tag | (more ? kHasWaiters : 0));
                next->mutex_addr = 0;
                Wake(next, 0);
            }
        }
        t->cv_key = key;
        t->mutex_addr = maddr;
        t->mutex_tag = tag;
        cv_waiters.push_back(t);
        AtomicStore(m, key, 1);
        Result r = WaitCurrent(lk, t, timeout);
        if (r == rc::TimedOut) {
            // Still on the condvar list (not signalled) or on the lock list (signalled but mutex busy).
            const bool on_cv = std::find(cv_waiters.begin(), cv_waiters.end(), t) != cv_waiters.end();
            Erase(cv_waiters, t);
            if (on_cv) {
                if (std::none_of(cv_waiters.begin(), cv_waiters.end(), [&](Thread* w) { return w->cv_key == key; }))
                    AtomicStore(m, key, 0);
            } else {
                // Moved to lock waiters by SignalProcessWideKey: wait for the hand-off.
                r = WaitCurrent(lk, t, -1);
                Erase(lock_waiters, t);
            }
        }
        t->cv_key = 0;
        Ret(r);
        break;
    }
    case 0x1D: { // SignalProcessWideKey(key, count)
        const u64 key = R(0);
        s32 count = s32(R(1));
        std::lock_guard lk(lock);
        while (count != 0) {
            Thread* w = HighestPriority(cv_waiters, [&](Thread* x) { return x->cv_key == key; });
            if (!w)
                break;
            Erase(cv_waiters, w);
            w->cv_key = 0;
            // Try to hand the mutex to the waiter.
            const u64 maddr = w->mutex_addr;
            u32 cur = AtomicLoad(m, maddr);
            for (;;) {
                if (cur == 0) {
                    if (AtomicCas(m, maddr, cur, w->mutex_tag)) {
                        w->mutex_addr = 0;
                        Wake(w, 0);
                        break;
                    }
                } else {
                    if ((cur & kHasWaiters) || AtomicCas(m, maddr, cur, cur | kHasWaiters)) {
                        auto owner = GetThread(cur & ~kHasWaiters);
                        w->lock_owner = owner.get();
                        lock_waiters.push_back(w);
                        break;
                    }
                }
            }
            if (count > 0)
                count--;
        }
        if (std::none_of(cv_waiters.begin(), cv_waiters.end(), [&](Thread* x) { return x->cv_key == key; }))
            AtomicStore(m, key, 0);
        break;
    }
    case 0x1E: // GetSystemTick
        W(0, GetTicks());
        break;
    case 0x1F: { // ConnectToNamedPort(_, name)
        const std::string name = m.ReadString(R(1), 12);
        auto session = services_->ConnectToPort(name);
        if (!session) {
            LOG_ERROR("SVC", "ConnectToNamedPort: unknown port '{}'", name);
            Ret(rc::NotFound);
            break;
        }
        W(1, handles_.Add(session));
        Ret(0);
        break;
    }
    case 0x21: // SendSyncRequest(handle)
        Ret(services_->HandleSyncRequest(t, Handle(R(0)), t->tls, 0x100));
        break;
    case 0x22: // SendSyncRequestWithUserBuffer(buf, size, handle)
        Ret(services_->HandleSyncRequest(t, Handle(R(2)), R(0), R(1)));
        break;
    case 0x24: // GetProcessId(_, handle)
        W(1, 0x51);
        Ret(0);
        break;
    case 0x25: { // GetThreadId(_, handle)
        auto th = GetThread(Handle(R(1)));
        if (!th) {
            Ret(rc::InvalidHandle);
            break;
        }
        W(1, th->id);
        Ret(0);
        break;
    }
    case 0x26: { // Break(reason, info, size)
        const u64 reason = R(0);
        LOG_ERROR("SVC", "svcBreak(reason={:#x}, info={:#x}, size={:#x})", reason, R(1), R(2));
        if (R(2) == 4 && m.IsValid(R(1), 4))
            LOG_ERROR("SVC", "  break info word: {:#x}", m.Read<u32>(R(1)));
        DumpThreadState(t);
        if (reason & 0x80000000) { // notification only
            Ret(0);
            break;
        }
        PANIC("Guest called svcBreak");
    }
    case 0x27: { // OutputDebugString(str, size)
        std::string s(size_t(R(1)), '\0');
        m.ReadBlock(R(0), s.data(), s.size());
        while (!s.empty() && (s.back() == '\n' || s.back() == '\0'))
            s.pop_back();
        LOG_INFO("Guest", "{}", s);
        Ret(0);
        break;
    }
    case 0x28: // ReturnFromException
        LOG_ERROR("SVC", "ReturnFromException({:#x})", R(0));
        DumpThreadState(t);
        PANIC("Guest exception handler returned");
    case 0x29: { // GetInfo(_, type, handle, subtype)
        const u32 type = u32(R(1));
        const u64 sub = R(3);
        u64 out = 0;
        Result r = 0;
        switch (type) {
        case 0: out = 0xF; break;                          // core mask
        case 1: out = 0xFFFFFFFFFFFFFFFFull; break;        // priority mask
        case 2: out = layout::kAliasBase; break;
        case 3: out = layout::kAliasSize; break;
        case 4: out = layout::kHeapBase; break;
        case 5: out = layout::kHeapSize; break;
        case 6: out = layout::kTotalMemory; break;
        case 7: out = m.UsedMemory(); break;
        case 8: out = 0; break;                            // debugger attached
        case 9: out = 0; break;                            // resource limit handle
        case 10: out = 0; break;                           // idle ticks
        case 11: out = random_entropy[sub & 3]; break;
        case 12: out = layout::kAslrBase; break;
        case 13: out = layout::kAslrSize; break;
        case 14: out = layout::kStackBase; break;
        case 15: out = layout::kStackSize; break;
        case 16: out = 0; break;                           // system resource size
        case 17: out = 0; break;
        case 18: out = program_id; break;
        case 20: out = 0; break;                           // user exception context
        case 21: out = layout::kTotalMemory; break;
        case 22: out = m.UsedMemory(); break;
        case 0xF0000002: out = GetTicks(); break;          // thread tick count
        default:
            LOG_WARN("SVC", "GetInfo: unknown type {:#x} sub {:#x}", type, sub);
            r = rc::InvalidEnumValue;
        }
        W(1, out);
        Ret(r);
        break;
    }
    case 0x2A: // FlushEntireDataCache
    case 0x2B: // FlushDataCache
        Ret(0);
        break;
    case 0x2C: { // MapPhysicalMemory(addr, size)
        if (!m.Map(R(0), R(1), MS_Normal, MP_RW)) {
            Ret(rc::InvalidMemoryRange);
            break;
        }
        Ret(0);
        break;
    }
    case 0x2D: // UnmapPhysicalMemory
        m.Unmap(R(0), R(1));
        Ret(0);
        break;
    case 0x32: // SetThreadActivity
        LOG_WARN("SVC", "SetThreadActivity({:#x}, {}) ignored", R(0), R(1));
        Ret(0);
        break;
    case 0x34: { // WaitForAddress(addr, type, value, timeout)
        const u64 addr = R(0);
        const u32 type = u32(R(1));
        const s32 value = s32(R(2));
        const s64 timeout = s64(R(3));
        std::unique_lock lk(lock);
        s32 cur = s32(AtomicLoad(m, addr));
        bool wait = false;
        switch (type) {
        case 0: wait = cur < value; break; // WaitIfLessThan
        case 1:                            // DecrementAndWaitIfLessThan
            wait = cur < value;
            if (wait)
                AtomicStore(m, addr, u32(cur - 1));
            break;
        case 2: wait = cur == value; break; // WaitIfEqual
        default: Ret(rc::InvalidEnumValue); return;
        }
        if (!wait) {
            Ret(rc::InvalidState);
            break;
        }
        if (timeout == 0) {
            Ret(rc::TimedOut);
            break;
        }
        t->arbiter_addr = addr;
        arb_waiters.push_back(t);
        const Result r = WaitCurrent(lk, t, timeout);
        Erase(arb_waiters, t);
        t->arbiter_addr = 0;
        Ret(r);
        break;
    }
    case 0x35: { // SignalToAddress(addr, type, value, count)
        const u64 addr = R(0);
        const u32 type = u32(R(1));
        const s32 value = s32(R(2));
        const s32 count = s32(R(3));
        std::lock_guard lk(lock);
        std::vector<Thread*> ws;
        for (Thread* w : arb_waiters)
            if (w->arbiter_addr == addr)
                ws.push_back(w);
        std::stable_sort(ws.begin(), ws.end(), [](Thread* a, Thread* b) { return a->priority < b->priority; });
        if (type == 1 || type == 2) {
            const s32 cur = s32(AtomicLoad(m, addr));
            if (cur != value) {
                Ret(rc::InvalidState);
                break;
            }
            s32 nv = value + 1;
            if (type == 2) {
                if (ws.empty())
                    nv = value + 1;
                else if (count <= 0 || s32(ws.size()) <= count)
                    nv = value - 1;
                else
                    nv = value;
            }
            AtomicStore(m, addr, u32(nv));
        }
        s32 n = 0;
        for (Thread* w : ws) {
            if (count > 0 && n >= count)
                break;
            Erase(arb_waiters, w);
            w->arbiter_addr = 0;
            Wake(w, 0);
            n++;
        }
        Ret(0);
        break;
    }
    case 0x45: { // CreateEvent -> (write handle, read handle)
        auto e = std::make_shared<Event>("user");
        W(1, handles_.Add(e));
        W(2, handles_.Add(e));
        Ret(0);
        break;
    }
    default:
        LOG_ERROR("SVC", "Unimplemented SVC {:#x} (pc {:#x})", id, c.PC());
        DumpThreadState(t);
        Ret(rc::NotImplemented);
        break;
    }
}

} // namespace kernel
