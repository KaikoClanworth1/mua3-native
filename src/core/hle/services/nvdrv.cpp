// nvdrv: /dev/nvmap, nvhost-as-gpu, nvhost-gpu, nvhost-ctrl, nvhost-ctrl-gpu.
#include <map>
#include <unordered_map>

#include "core/hle/nvmap.h"
#include "core/hle/service.h"
#include "gpu/gpu.h"

namespace hle {

namespace nv {

constexpr u32 kSuccess = 0;
constexpr u32 kNotImplemented = 1;
constexpr u32 kBadParameter = 4;
constexpr u32 kTimeout = 5;
constexpr u32 kConfigVarNotFound = 0x30006;

// ---------------------------------------------------------------- nvmap objects
NvMap& Map() {
    static NvMap m;
    return m;
}

u32 NvMap::Create(u32 size) {
    std::lock_guard lk(mutex);
    auto o = std::make_shared<Object>();
    o->id = next_id++;
    o->handle = next_handle++;
    o->size = size;
    by_handle[o->handle] = o;
    by_id[o->id] = o;
    return o->handle;
}

std::shared_ptr<NvMap::Object> NvMap::Get(u32 handle) {
    std::lock_guard lk(mutex);
    auto it = by_handle.find(handle);
    return it == by_handle.end() ? nullptr : it->second;
}

std::shared_ptr<NvMap::Object> NvMap::GetById(u32 id) {
    std::lock_guard lk(mutex);
    auto it = by_id.find(id);
    return it == by_id.end() ? nullptr : it->second;
}

namespace {

// Second input buffer of Ioctl2 (GPFIFO entries for KICKOFF_PB_WITH_ATTR).
thread_local const std::vector<u8>* t_inline_in = nullptr;

struct Device {
    virtual ~Device() = default;
    virtual u32 Ioctl(u32 cmd, std::vector<u8>& in, std::vector<u8>& out) = 0;
    virtual std::shared_ptr<kernel::Event> QueryEvent(u32) { return nullptr; }
    std::string name;
};

template <typename T>
T Arg(const std::vector<u8>& v, size_t off = 0) {
    T x{};
    if (off + sizeof(T) <= v.size())
        std::memcpy(&x, v.data() + off, sizeof(T));
    return x;
}
template <typename T>
void Set(std::vector<u8>& v, size_t off, const T& x) {
    if (v.size() < off + sizeof(T))
        v.resize(off + sizeof(T));
    std::memcpy(v.data() + off, &x, sizeof(T));
}

// ---------------------------------------------------------------- /dev/nvmap
struct NvMapDev final : Device {
    u32 Ioctl(u32 cmd, std::vector<u8>& in, std::vector<u8>& out) override {
        out = in;
        auto& m = Map();
        switch (cmd & 0xFF) {
        case 0x01: { // CREATE {u32 size, u32 handle}
            Set<u32>(out, 4, m.Create(Arg<u32>(in, 0)));
            return kSuccess;
        }
        case 0x03: { // FROM_ID {u32 id, u32 handle}
            auto o = m.GetById(Arg<u32>(in, 0));
            if (!o)
                return kBadParameter;
            Set<u32>(out, 4, o->handle);
            return kSuccess;
        }
        case 0x04: { // ALLOC {handle, heap_mask, flags, align, kind, pad[7], addr}
            auto o = m.Get(Arg<u32>(in, 0));
            if (!o)
                return kBadParameter;
            o->align = Arg<u32>(in, 12);
            o->kind = Arg<u8>(in, 16);
            o->addr = Arg<u64>(in, 24);
            o->allocated = true;
            return kSuccess;
        }
        case 0x05: { // FREE {handle, pad, address, size, flags}
            auto o = m.Get(Arg<u32>(in, 0));
            if (!o)
                return kBadParameter;
            Set<u64>(out, 8, o->addr);
            Set<u32>(out, 16, o->size);
            Set<u32>(out, 20, 0);
            return kSuccess;
        }
        case 0x09: { // PARAM {handle, param, result}
            auto o = m.Get(Arg<u32>(in, 0));
            if (!o)
                return kBadParameter;
            u32 v = 0;
            switch (Arg<u32>(in, 4)) {
            case 1: v = o->size; break;
            case 2: v = o->align; break;
            case 3: v = 0x40000000; break; // base (heap)
            case 4: v = 0x40000000; break; // heap mask
            case 5: v = o->kind; break;
            case 6: v = 0; break;
            }
            Set<u32>(out, 8, v);
            return kSuccess;
        }
        case 0x0E: { // GET_ID {u32 id, u32 handle}
            auto o = m.Get(Arg<u32>(in, 4));
            if (!o)
                return kBadParameter;
            Set<u32>(out, 0, o->id);
            return kSuccess;
        }
        }
        LOG_WARN("nvdrv", "nvmap: unhandled ioctl {:#x}", cmd);
        return kSuccess;
    }
};

// ---------------------------------------------------------------- /dev/nvhost-as-gpu
struct AsGpuDev final : Device {
    u32 Ioctl(u32 cmd, std::vector<u8>& in, std::vector<u8>& out) override {
        out = in;
        auto& as = gpu::G().As();
        switch (cmd & 0xFF) {
        case 0x01: return kSuccess; // BIND_CHANNEL
        case 0x02: {                // ALLOC_SPACE {pages, page_size, flags, pad, offset}
            const u32 pages = Arg<u32>(in, 0), page_size = Arg<u32>(in, 4), flags = Arg<u32>(in, 8);
            const u64 size = u64(pages) * page_size;
            u64 va;
            if (flags & 1) { // fixed
                va = Arg<u64>(in, 16);
                as.Reserve(va, size);
            } else {
                va = as.Allocate(size, page_size);
            }
            Set<u64>(out, 16, va);
            return kSuccess;
        }
        case 0x03: return kSuccess; // FREE_SPACE
        case 0x05: {                // UNMAP_BUFFER {offset}
            const u64 va = Arg<u64>(in, 0);
            auto it = buffers_.find(va);
            if (it != buffers_.end()) {
                as.Unmap(va, it->second.size);
                buffers_.erase(it);
            }
            return kSuccess;
        }
        case 0x06: { // MAP_BUFFER_EX {flags, kind, handle, page_size, buffer_offset, mapping_size, offset}
            const u32 flags = Arg<u32>(in, 0), handle = Arg<u32>(in, 8);
            const u64 buf_off = Arg<u64>(in, 16);
            u64 size = Arg<u64>(in, 24);
            u64 va = Arg<u64>(in, 32);
            if (flags & 0x100) { // remap a sub-range of an existing mapping (handle unused)
                auto it = buffers_.upper_bound(va);
                if (it == buffers_.begin()) {
                    LOG_ERROR("nvdrv", "MAP_BUFFER_EX remap: no mapping at {:#x}", va);
                    return kBadParameter;
                }
                --it;
                as.Map(va + buf_off, it->second.cpu + buf_off, size);
                return kSuccess;
            }
            auto o = Map().Get(handle);
            if (!o) {
                LOG_ERROR("nvdrv", "MAP_BUFFER_EX: bad nvmap handle {} (flags {:#x})", handle, flags);
                return kBadParameter;
            }
            if (!size)
                size = o->size - buf_off;
            if (!(flags & 1))
                va = as.Allocate(size, std::max<u32>(Arg<u32>(in, 12), 0x10000));
            as.Map(va, o->addr + buf_off, size);
            buffers_[va] = {size, o->addr + buf_off};
            Set<u64>(out, 32, va);
            LOG_TRACE("nvdrv", "MAP_BUFFER_EX handle {} cpu {:#x}+{:#x} -> gpu {:#x}", handle, o->addr + buf_off,
                      size, va);
            return kSuccess;
        }
        case 0x08: { // GET_VA_REGIONS
            Set<u32>(out, 8, 0x30);
            // small pages
            Set<u64>(out, 0x10, 0x04000000ull);
            Set<u32>(out, 0x18, 0x1000);
            Set<u64>(out, 0x20, 0x3FBFFull);
            // big pages
            Set<u64>(out, 0x28, 0x0400000000ull);
            Set<u32>(out, 0x30, 0x10000);
            Set<u64>(out, 0x38, 0x1BFFFF8ull);
            return kSuccess;
        }
        case 0x09: return kSuccess; // ALLOC_AS_EX / INITIALIZE_EX
        case 0x14: {                // REMAP: array of {u16 flags, u16 kind, u32 handle, u32 map_offset, u32 offset, u32 pages}
            for (size_t p = 0; p + 0x14 <= in.size(); p += 0x14) {
                const u32 handle = Arg<u32>(in, p + 4);
                const u64 map_off = u64(Arg<u32>(in, p + 8)) << 16;
                const u64 va = u64(Arg<u32>(in, p + 12)) << 16;
                const u64 size = u64(Arg<u32>(in, p + 16)) << 16;
                auto o = Map().Get(handle);
                if (o && handle)
                    as.Map(va, o->addr + map_off, size);
                else
                    as.Unmap(va, size);
            }
            return kSuccess;
        }
        }
        LOG_WARN("nvdrv", "nvhost-as-gpu: unhandled ioctl {:#x}", cmd);
        return kSuccess;
    }
    struct BufferMap {
        u64 size, cpu;
    };
    std::map<u64, BufferMap> buffers_;
};

// ---------------------------------------------------------------- /dev/nvhost-gpu (channel)
struct GpuChannelDev final : Device {
    explicit GpuChannelDev(u32 sp) : syncpt(sp) {}
    u32 syncpt;
    std::shared_ptr<kernel::Event> events[4] = {MakeEvent("nvgpu:sm"), MakeEvent("nvgpu:err"), MakeEvent("nvgpu:sm2"),
                                                 MakeEvent("nvgpu:3")};

    u32 Submit(std::vector<u8>& in, std::vector<u8>& out, size_t hdr_size, const u64* entries_override) {
        u32 n = Arg<u32>(in, 8);
        const u32 flags = Arg<u32>(in, 12);
        if (entries_override && t_inline_in)
            n = std::min<u32>(n, u32(t_inline_in->size() / 8));
        std::vector<u64> entries(n);
        if (entries_override)
            std::memcpy(entries.data(), entries_override, n * 8);
        else
            for (u32 i = 0; i < n; i++)
                entries[i] = Arg<u64>(in, hdr_size + i * 8);
        auto& g = gpu::G();
        // flags: bit0 wait on the input fence, bit1 the driver appends a syncpoint increment,
        // bit8 the pushbuffer itself increments the channel syncpoint by fence.value (NVN's mode).
        u32 driver_incr = 0, value;
        if (flags & (1u << 8)) {
            value = g.ReserveSyncpoint(syncpt, Arg<u32>(in, 20));
        } else if (flags & 2) {
            driver_incr = 1;
            value = g.ReserveSyncpoint(syncpt, 1);
        } else {
            value = g.SyncpointMax(syncpt);
        }
        g.Submit(std::move(entries), syncpt, driver_incr);
        out.resize(std::max<size_t>(out.size(), 0x18));
        std::memcpy(out.data(), in.data(), std::min<size_t>(0x18, in.size()));
        Set<u32>(out, 16, syncpt);
        Set<u32>(out, 20, value);
        return kSuccess;
    }

    u32 Ioctl(u32 cmd, std::vector<u8>& in, std::vector<u8>& out) override {
        out = in;
        const u32 type = (cmd >> 8) & 0xFF;
        const u32 nr = cmd & 0xFF;
        if (type == 0) { // nvhost generic
            switch (nr) {
            case 0x02: // GET_SYNCPOINT {param, value}
                Set<u32>(out, 4, syncpt);
                return kSuccess;
            case 0x03: // GET_WAITBASE
                Set<u32>(out, 4, 0);
                return kSuccess;
            case 0x09: return kSuccess; // SET_SUBMIT_TIMEOUT
            }
        }
        switch (nr) {
        case 0x01: return kSuccess; // SET_NVMAP_FD
        case 0x03: return kSuccess; // SET_TIMEOUT
        case 0x08: return Submit(in, out, 0x18, nullptr); // SUBMIT_GPFIFO
        case 0x09:                                        // ALLOC_OBJ_CTX {class, flags, obj_id}
            Set<u64>(out, 8, 0x1000 + Arg<u32>(in, 0));
            return kSuccess;
        case 0x0B: return kSuccess; // ZCULL_BIND
        case 0x0C: return kSuccess; // SET_ERROR_NOTIFIER
        case 0x0D: return kSuccess; // SET_PRIORITY
        case 0x12: return kSuccess; // ENABLE_COMMAND_BUFFER? / channel wdt
        case 0x14: return kSuccess; // SET_CLIENT_DATA
        case 0x15:                  // GET_CLIENT_DATA
            Set<u64>(out, 0, 0);
            return kSuccess;
        case 0x16: return kSuccess; // GET_ERROR_NOTIFICATION
        case 0x17: return kSuccess;
        case 0x1A: // ALLOC_GPFIFO_EX2 {num_entries, flags, unk0, fence_out{id,value}, unk1, unk2, unk3}
            Set<u32>(out, 12, syncpt);
            Set<u32>(out, 16, gpu::G().SyncpointMax(syncpt));
            return kSuccess;
        case 0x1B: // KICKOFF_PB_WITH_ATTR: entries come in the Ioctl2 inline buffer
            if (t_inline_in && !t_inline_in->empty())
                return Submit(in, out, 0x18, reinterpret_cast<const u64*>(t_inline_in->data()));
            return Submit(in, out, 0x18, nullptr);
        case 0x1D: return kSuccess;                       // SET_TIMESLICE
        }
        LOG_WARN("nvdrv", "nvhost-gpu: unhandled ioctl {:#x}", cmd);
        return kSuccess;
    }

    std::shared_ptr<kernel::Event> QueryEvent(u32 id) override { return events[std::min<u32>(id, 3)]; }
};

// ---------------------------------------------------------------- /dev/nvhost-ctrl
struct CtrlDev final : Device {
    std::shared_ptr<kernel::Event> events[64];
    CtrlDev() {
        for (auto& e : events)
            e = MakeEvent("nvctrl", true);
    }
    u32 Ioctl(u32 cmd, std::vector<u8>& in, std::vector<u8>& out) override {
        out = in;
        auto& g = gpu::G();
        switch (cmd & 0xFF) {
        case 0x14: // SYNCPT_READ {id, value}
            Set<u32>(out, 4, g.SyncpointValue(Arg<u32>(in, 0)));
            return kSuccess;
        case 0x1A: // SYNCPT_READ_MAX
            Set<u32>(out, 4, g.SyncpointMax(Arg<u32>(in, 0)));
            return kSuccess;
        case 0x15: // SYNCPT_INCR
            g.IncrementSyncpoint(Arg<u32>(in, 0));
            return kSuccess;
        case 0x16:   // SYNCPT_WAIT {id, thresh, timeout}
        case 0x19: { // SYNCPT_WAITEX {id, thresh, timeout, value}
            const u32 id = Arg<u32>(in, 0), thresh = Arg<u32>(in, 4);
            const s32 timeout = Arg<s32>(in, 8);
            if (!g.WaitSyncpoint(id, thresh, timeout < 0 ? -1 : s64(timeout) * 1000)) {
                LOG_DEBUG("nvdrv", "syncpoint wait timeout (ioctl {:#x}): id {} thresh {} value {} max {} (timeout {}) raw {}",
                          cmd, id, thresh, g.SyncpointValue(id), g.SyncpointMax(id), timeout, common::ToHex(in));
                return kTimeout;
            }
            Set<u32>(out, 12, g.SyncpointValue(id));
            return kSuccess;
        }
        case 0x1B: { // GET_CONFIG
            const std::string domain(reinterpret_cast<const char*>(in.data()), strnlen((const char*)in.data(), 0x41));
            const std::string param(reinterpret_cast<const char*>(in.data() + 0x41),
                                    strnlen((const char*)in.data() + 0x41, 0x41));
            LOG_DEBUG("nvdrv", "GET_CONFIG {}!{}", domain, param);
            return kConfigVarNotFound;
        }
        case 0x1D:   // EVENT_WAIT {fence id, thresh, timeout, value}
        case 0x1E: { // EVENT_WAIT_ASYNC
            const u32 id = Arg<u32>(in, 0), thresh = Arg<u32>(in, 4);
            const s32 timeout = Arg<s32>(in, 8);
            if (!g.WaitSyncpoint(id, thresh, timeout < 0 ? -1 : s64(timeout) * 1000)) {
                LOG_DEBUG("nvdrv", "syncpoint wait timeout (ioctl {:#x}): id {} thresh {} value {} max {} (timeout {}) raw {}",
                          cmd, id, thresh, g.SyncpointValue(id), g.SyncpointMax(id), timeout, common::ToHex(in));
                return kTimeout;
            }
            Set<u32>(out, 12, g.SyncpointValue(id));
            return kSuccess;
        }
        case 0x1C: return kSuccess; // CLEAR_EVENT_WAIT
        case 0x1F: return kSuccess; // EVENT_REGISTER
        case 0x20: return kSuccess; // EVENT_UNREGISTER
        case 0x21: return kSuccess; // EVENT_KILL
        }
        LOG_WARN("nvdrv", "nvhost-ctrl: unhandled ioctl {:#x}", cmd);
        return kSuccess;
    }
    std::shared_ptr<kernel::Event> QueryEvent(u32 id) override { return events[id & 63]; }
};

// ---------------------------------------------------------------- /dev/nvhost-ctrl-gpu
struct CtrlGpuDev final : Device {
    std::shared_ptr<kernel::Event> events[2] = {MakeEvent("ctrlgpu0"), MakeEvent("ctrlgpu1")};
    u32 Ioctl(u32 cmd, std::vector<u8>& in, std::vector<u8>& out) override {
        out = in;
        switch (cmd & 0xFF) {
        case 0x01: // ZCULL_GET_CTX_SIZE
            Set<u32>(out, 0, 1);
            return kSuccess;
        case 0x02: { // ZCULL_GET_INFO
            const u32 v[10] = {0x20, 0x20, 0x400, 0x800, 0x20, 0x20, 0xC0, 0x20, 0x40, 0x10};
            out.assign((u8*)v, (u8*)v + sizeof(v));
            return kSuccess;
        }
        case 0x03: return kSuccess; // ZBC_SET_TABLE
        case 0x04: return kSuccess; // ZBC_QUERY_TABLE
        case 0x05: {                // GET_CHARACTERISTICS
            std::vector<u8> c(0xA0);
            size_t p = 0;
            auto w32 = [&](u32 v) { std::memcpy(&c[p], &v, 4), p += 4; };
            auto w64 = [&](u64 v) { std::memcpy(&c[p], &v, 8), p += 8; };
            w32(0x120); w32(0xB); w32(0xA1); w32(1);
            w64(0x40000); w64(0);
            w32(2); w32(0x20); w32(0x20000); w32(0x20000); w32(27); w32(0x30000); w32(1); w32(0x503); w32(0x503);
            w32(0x80); w32(40); w32(0);
            w64(0x55);
            w32(0x902D); w32(0xB197); w32(0xB1C0); w32(0xB06F); w32(0xA140); w32(0xB0B5); w32(1); w32(0); w32(2);
            w32(1); w32(0); w32(1); w32(0x21D70); w32(0);
            w64(0x6230326D67ull); w64(0);
            out.resize(std::max<size_t>(in.size(), 0x10 + 0xA0));
            Set<u64>(out, 0, 0xA0);
            std::memcpy(out.data() + 0x10, c.data(), 0xA0);
            return kSuccess;
        }
        case 0x06: // GET_TPC_MASKS
            Set<u32>(out, 0x10, 0x3);
            return kSuccess;
        case 0x07: return kSuccess; // FLUSH_L2
        case 0x14:                  // GET_ACTIVE_SLOT_MASK
            Set<u32>(out, 0, 0x7);
            Set<u32>(out, 4, 0x1);
            return kSuccess;
        case 0x1C: // GET_GPU_TIME
            Set<u64>(out, 0, u64(kernel::K().GetTicks() * 1000 / 19200 * 1000));
            return kSuccess;
        }
        LOG_WARN("nvdrv", "nvhost-ctrl-gpu: unhandled ioctl {:#x}", cmd);
        return kSuccess;
    }
    std::shared_ptr<kernel::Event> QueryEvent(u32 id) override { return events[id & 1]; }
};

struct NullDev final : Device {
    u32 Ioctl(u32 cmd, std::vector<u8>& in, std::vector<u8>& out) override {
        out = in;
        LOG_WARN("nvdrv", "{}: ioctl {:#x} ignored", name, cmd);
        return kSuccess;
    }
};

struct State {
    std::mutex mutex;
    std::unordered_map<u32, std::shared_ptr<Device>> fds;
    u32 next_fd = 1;
    u32 next_syncpt = 18;
};
State& St() {
    static State s;
    return s;
}

class NvdrvService final : public Service {
public:
    explicit NvdrvService(std::string n) : Service(std::move(n)) {
        Reg(0, "Open", [](Request& r) {
            const std::string path = r.ReadInString(0);
            std::shared_ptr<Device> dev;
            auto& s = St();
            std::lock_guard lk(s.mutex);
            if (path == "/dev/nvmap")
                dev = std::make_shared<NvMapDev>();
            else if (path == "/dev/nvhost-as-gpu")
                dev = std::make_shared<AsGpuDev>();
            else if (path == "/dev/nvhost-gpu")
                dev = std::make_shared<GpuChannelDev>(s.next_syncpt++);
            else if (path == "/dev/nvhost-ctrl")
                dev = std::make_shared<CtrlDev>();
            else if (path == "/dev/nvhost-ctrl-gpu")
                dev = std::make_shared<CtrlGpuDev>();
            else
                dev = std::make_shared<NullDev>();
            dev->name = path;
            const u32 fd = s.next_fd++;
            s.fds[fd] = dev;
            LOG_INFO("nvdrv", "Open {} -> fd {}", path, fd);
            r.Push<u32>(fd);
            r.Push<u32>(kSuccess);
        });
        auto ioctl = [](Request& r) {
            const u32 fd = r.ArgAt<u32>(0), cmd = r.ArgAt<u32>(4);
            std::shared_ptr<Device> dev;
            {
                std::lock_guard lk(St().mutex);
                auto it = St().fds.find(fd);
                if (it != St().fds.end())
                    dev = it->second;
            }
            if (!dev) {
                r.Push<u32>(kBadParameter);
                return;
            }
            auto in = r.ReadIn(0);
            std::vector<u8> inline_in;
            if (r.cmd == 11)
                inline_in = r.ReadIn(1);
            t_inline_in = &inline_in;
            std::vector<u8> out;
            const u32 res = dev->Ioctl(cmd, in, out);
            t_inline_in = nullptr;
            LOG_TRACE("nvdrv", "{} ioctl {:#x} -> {:#x}", dev->name, cmd, res);
            r.WriteOut(out);
            r.Push<u32>(res);
        };
        Reg(1, "Ioctl", ioctl);
        Reg(11, "Ioctl2", ioctl);
        Reg(12, "Ioctl3", ioctl);
        Reg(2, "Close", [](Request& r) {
            std::lock_guard lk(St().mutex);
            St().fds.erase(r.ArgAt<u32>(0));
            r.Push<u32>(kSuccess);
        });
        Reg(3, "Initialize", [](Request& r) { r.Push<u32>(kSuccess); });
        Reg(4, "QueryEvent", [](Request& r) {
            const u32 fd = r.ArgAt<u32>(0), id = r.ArgAt<u32>(4);
            std::shared_ptr<Device> dev;
            {
                std::lock_guard lk(St().mutex);
                auto it = St().fds.find(fd);
                if (it != St().fds.end())
                    dev = it->second;
            }
            auto e = dev ? dev->QueryEvent(id) : nullptr;
            if (!e) {
                LOG_WARN("nvdrv", "QueryEvent fd {} id {:#x}: none", fd, id);
                r.Push<u32>(kBadParameter);
                return;
            }
            r.PushCopy(e);
            r.Push<u32>(kSuccess);
        });
        Reg(8, "SetAruid", [](Request& r) { r.Push<u32>(kSuccess); });
        Reg(13, "SetGraphicsFirmwareMemoryMarginEnabled", [](Request&) {});
        quiet_ = true;
    }
};

} // namespace
} // namespace nv

void RegisterNvdrv(ServiceManager& sm) {
    for (const char* n : {"nvdrv", "nvdrv:a", "nvdrv:s", "nvdrv:t"})
        sm.RegisterService(n, [n] { return Make<nv::NvdrvService>(n); });
    sm.RegisterService("nvmemp", [] { return MakeStub("nvmemp"); });
}

} // namespace hle
