// vi: displays, layers and the IGraphicBufferProducer binder (NVN's swapchain).
#include <chrono>
#include <thread>

#include <windows.h>

#include "core/hle/nvmap.h"
#include "core/hle/service.h"
#include "core/system.h"
#include "gpu/gpu.h"

namespace hle {

namespace {

// ---------------------------------------------------------------- Parcel
class ParcelReader {
public:
    explicit ParcelReader(std::vector<u8> d) : buf_(std::move(d)) {
        if (buf_.size() >= 16) {
            const u32 size = common::Read<u32>(&buf_[0]), off = common::Read<u32>(&buf_[4]);
            if (off <= buf_.size())
                data_ = std::vector<u8>(buf_.begin() + off, buf_.begin() + std::min<size_t>(off + size, buf_.size()));
        }
    }
    template <typename T>
    T Read() {
        T v{};
        if (pos_ + sizeof(T) <= data_.size())
            std::memcpy(&v, &data_[pos_], sizeof(T));
        pos_ += common::AlignUp(sizeof(T), 4);
        return v;
    }
    void Skip(size_t n) { pos_ += common::AlignUp(n, 4); }
    std::u16string ReadString16() {
        const s32 len = Read<s32>();
        std::u16string s;
        if (len > 0 && pos_ + size_t(len) * 2 <= data_.size())
            s.assign(reinterpret_cast<const char16_t*>(&data_[pos_]), size_t(len));
        pos_ += common::AlignUp(size_t(std::max(len, 0) + 1) * 2, 4);
        return s;
    }
    void ReadInterfaceToken() {
        Read<u32>(); // strict mode
        ReadString16();
    }
    // Flattened object: u32 len, u32 fd count, data
    std::vector<u8> ReadFlattened() {
        const u32 len = Read<u32>();
        Read<u32>();
        std::vector<u8> v;
        if (pos_ + len <= data_.size())
            v.assign(data_.begin() + pos_, data_.begin() + pos_ + len);
        pos_ += common::AlignUp(len, 4);
        return v;
    }

private:
    std::vector<u8> buf_, data_;
    size_t pos_ = 0;
};

class ParcelWriter {
public:
    template <typename T>
    void Write(const T& v) {
        const size_t p = data_.size();
        data_.resize(p + common::AlignUp(sizeof(T), 4));
        std::memcpy(&data_[p], &v, sizeof(T));
    }
    void WriteRaw(const void* d, size_t n) {
        const size_t p = data_.size();
        data_.resize(p + common::AlignUp(n, 4));
        std::memcpy(&data_[p], d, n);
    }
    void WriteFlattened(const void* d, u32 n) {
        Write<u32>(n);
        Write<u32>(0);
        WriteRaw(d, n);
    }
    std::vector<u8> Finish() const {
        // header | data | objects (one empty u32 entry, as the official binder emits)
        std::vector<u8> out(0x10 + data_.size() + 4);
        const u32 hdr[4] = {u32(data_.size()), 0x10, 4, u32(0x10 + data_.size())};
        std::memcpy(out.data(), hdr, 16);
        std::memcpy(out.data() + 16, data_.data(), data_.size());
        return out;
    }

private:
    std::vector<u8> data_;
};

// ---------------------------------------------------------------- Buffer queue
struct BufferQueue {
    struct Slot {
        bool preallocated = false;
        bool dequeued = false;
        std::vector<u8> graphic_buffer;
        u32 width = 0, height = 0, stride = 0, format = 0, nvmap = 0, offset = 0;
    };
    std::mutex mutex;
    Slot slots[64];
    u32 next = 0;
    std::shared_ptr<kernel::Event> wait_event = MakeEvent("vi:bufferqueue", true);
    std::chrono::steady_clock::time_point next_vsync = std::chrono::steady_clock::now();
    u64 frames = 0;
    std::chrono::steady_clock::time_point fps_t0 = std::chrono::steady_clock::now();
};

BufferQueue& Queue() {
    static BufferQueue q;
    return q;
}

void PaceFrame(BufferQueue& q, u32 swap_interval) {
    // With its frame rate unlocked (fps_target, see game_patches.cpp) the game's own limiter paces it; a second
    // independent 60 Hz pacer here would drift against it and hold back a frame now and then (judder).
    const auto& cfg = core::Sys().Cfg();
    if (cfg.fps_target > 30 && cfg.swap_interval == 0)
        return;
    const int limit = cfg.fps_limit;
    if (limit <= 0)
        return;
    using namespace std::chrono;
    const auto period = duration_cast<steady_clock::duration>(duration<double>(std::max<u32>(swap_interval, 1) / double(limit)));
    auto now = steady_clock::now();
    if (q.next_vsync < now - period * 2)
        q.next_vsync = now;
    q.next_vsync += period;
    while ((now = steady_clock::now()) < q.next_vsync) {
        const auto left = q.next_vsync - now;
        if (left > milliseconds(2))
            std::this_thread::sleep_for(left - milliseconds(1));
        else
            SwitchToThread();
    }
}

void Transact(u32 code, std::vector<u8> in, ParcelWriter& out) {
    auto& q = Queue();
    ParcelReader p(std::move(in));
    p.ReadInterfaceToken();
    switch (code) {
    case 1: { // RequestBuffer
        const s32 slot = p.Read<s32>();
        std::lock_guard lk(q.mutex);
        auto& s = q.slots[slot & 63];
        out.Write<s32>(s.graphic_buffer.empty() ? 0 : 1);
        if (!s.graphic_buffer.empty())
            out.WriteFlattened(s.graphic_buffer.data(), u32(s.graphic_buffer.size()));
        out.Write<s32>(0);
        break;
    }
    case 2: // SetBufferCount
        out.Write<s32>(0);
        break;
    case 3: { // DequeueBuffer
        std::lock_guard lk(q.mutex);
        s32 slot = -1;
        for (u32 i = 0; i < 64; i++) {
            const u32 idx = (q.next + i) % 64;
            if (q.slots[idx].preallocated && !q.slots[idx].dequeued) {
                slot = s32(idx);
                break;
            }
        }
        if (slot < 0)
            slot = 0;
        q.slots[slot].dequeued = true;
        q.next = (slot + 1) % 64;
        out.Write<s32>(slot);
        out.Write<u32>(1);
        u8 fence[0x24]{};
        out.WriteFlattened(fence, sizeof(fence));
        out.Write<s32>(0);
        break;
    }
    case 7: { // QueueBuffer
        const s32 slot = p.Read<s32>();
        auto qbi = p.ReadFlattened();
        u32 swap_interval = qbi.size() >= 0x30 ? common::Read<u32>(&qbi[0x2C]) : 1;
        // The game asks NVN for interval 2 (30 fps); with its frame rate unlocked (fps_target) present every frame.
        if (core::Sys().Cfg().fps_target > 30)
            swap_interval = 1;
        if (const int forced = core::Sys().Cfg().swap_interval; forced > 0)
            swap_interval = u32(forced);
        BufferQueue::Slot s;
        {
            std::lock_guard lk(q.mutex);
            q.slots[slot & 63].dequeued = false;
            s = q.slots[slot & 63];
        }
        u64 cpu = 0;
        if (auto o = nv::Map().Get(s.nvmap))
            cpu = o->addr + s.offset;
        else if (auto o2 = nv::Map().GetById(s.nvmap))
            cpu = o2->addr + s.offset;
        gpu::G().QueuePresent({cpu, s.width, s.height, s.stride, s.format});
        PaceFrame(q, swap_interval);
        q.frames++;
        const auto now = std::chrono::steady_clock::now();
        if (now - q.fps_t0 > std::chrono::seconds(5)) {
            const double secs = std::chrono::duration<double>(now - q.fps_t0).count();
            auto& g = gpu::G();
            LOG_INFO("vi", "Presenting: {:.1f} fps (swap interval {}, gpu submits {}, methods {}) gpu busy {:.0f}% (draw {:.0f}%)",
                     q.frames / secs, swap_interval, g.submits.load(), g.methods.load(), g.busy_ns.exchange(0) / (secs * 1e7), g.draw_ns.exchange(0) / (secs * 1e7));
            q.frames = 0;
            q.fps_t0 = now;
        }
        const u32 qbo[4] = {s.width ? s.width : 1920, s.height ? s.height : 1080, 0, 0};
        out.WriteRaw(qbo, sizeof(qbo));
        out.Write<s32>(0);
        kernel::K().SignalEvent(*q.wait_event);
        break;
    }
    case 8: { // CancelBuffer
        const s32 slot = p.Read<s32>();
        std::lock_guard lk(q.mutex);
        q.slots[slot & 63].dequeued = false;
        out.Write<s32>(0);
        break;
    }
    case 9: { // Query
        const s32 what = p.Read<s32>();
        s32 v = 0;
        switch (what) {
        case 0: v = 1920; break; // width
        case 1: v = 1080; break; // height
        case 2: v = 1; break;    // format RGBA8888
        case 3: v = 1; break;    // min undequeued
        case 5: v = 1; break;    // consumer running behind
        default: v = 0;
        }
        out.Write<s32>(v);
        out.Write<s32>(0);
        break;
    }
    case 10: { // Connect
        const bool docked = core::Sys().Cfg().docked;
        const u32 qbo[4] = {docked ? 1920u : 1280u, docked ? 1080u : 720u, 0, 0};
        out.WriteRaw(qbo, sizeof(qbo));
        out.Write<s32>(0);
        break;
    }
    case 11: // Disconnect
        out.Write<s32>(0);
        break;
    case 14: { // SetPreallocatedBuffer
        const s32 slot = p.Read<s32>();
        const s32 has = p.Read<s32>();
        std::lock_guard lk(q.mutex);
        auto& s = q.slots[slot & 63];
        if (has) {
            s.graphic_buffer = p.ReadFlattened();
            const auto& g = s.graphic_buffer;
            if (g.size() >= 0x7C) {
                s.width = common::Read<u32>(&g[4]);
                s.height = common::Read<u32>(&g[8]);
                s.stride = common::Read<u32>(&g[0xC]);
                s.format = common::Read<u32>(&g[0x10]);
                s.nvmap = common::Read<u32>(&g[0x74]);
                s.offset = common::Read<u32>(&g[0x78]);
            }
            s.preallocated = true;
            LOG_INFO("vi", "SetPreallocatedBuffer slot {} {}x{} stride {} fmt {} nvmap {} off {:#x}", slot, s.width,
                     s.height, s.stride, s.format, s.nvmap, s.offset);
        } else {
            s = {};
        }
        out.Write<s32>(0);
        break;
    }
    default:
        LOG_WARN("vi", "Unhandled binder transaction {}", code);
        out.Write<s32>(0);
    }
}

class IHOSBinderDriver final : public Service {
public:
    IHOSBinderDriver() : Service("vi:IHOSBinderDriver") {
        quiet_ = true;
        auto transact = [](Request& r) {
            const u32 code = r.ArgAt<u32>(4);
            ParcelWriter out;
            Transact(code, r.ReadIn(0), out);
            r.WriteOut(out.Finish());
        };
        Reg(0, "TransactParcel", transact);
        Reg(3, "TransactParcelAuto", transact);
        Ok(1, "AdjustRefcount");
        Reg(2, "GetNativeHandle", [](Request& r) { r.PushCopy(Queue().wait_event); });
    }
};

std::shared_ptr<kernel::Event> VsyncEvent() {
    static std::shared_ptr<kernel::Event> e = [] {
        auto ev = MakeEvent("vi:vsync");
        // MUA3_VSYNC_HZ: experiment with the display refresh the guest sees (default 60).
        const char* hz_env = std::getenv("MUA3_VSYNC_HZ");
        const int hz = hz_env ? std::max(1, std::atoi(hz_env)) : 60;
        std::thread([ev, hz] {
            for (;;) {
                std::this_thread::sleep_for(std::chrono::microseconds(1000000 / hz));
                kernel::K().SignalEvent(*ev);
            }
        }).detach();
        return ev;
    }();
    return e;
}

std::vector<u8> NativeWindowParcel(u32 binder_id) {
    ParcelWriter w;
    u8 nw[0x28]{};
    const u32 magic = 2, pid = 1;
    std::memcpy(nw, &magic, 4);
    std::memcpy(nw + 4, &pid, 4);
    std::memcpy(nw + 8, &binder_id, 4);
    std::memcpy(nw + 0x18, "dispdrv", 8);
    w.WriteRaw(nw, sizeof(nw));
    return w.Finish();
}

class IApplicationDisplayService final : public Service {
public:
    IApplicationDisplayService() : Service("vi:IApplicationDisplayService") {
        Reg(100, "GetRelayService", [](Request& r) { r.PushInterface(Make<IHOSBinderDriver>()); });
        Reg(101, "GetSystemDisplayService", [](Request& r) { r.PushInterface(MakeStub("vi:ISystemDisplayService")); });
        Reg(102, "GetManagerDisplayService",
            [](Request& r) { r.PushInterface(MakeStub("vi:IManagerDisplayService")); });
        Reg(103, "GetIndirectDisplayTransactionService",
            [](Request& r) { r.PushInterface(Make<IHOSBinderDriver>()); });
        Reg(1000, "ListDisplays", [](Request& r) {
            u8 info[0x60]{};
            std::strcpy(reinterpret_cast<char*>(info), "Default");
            info[0x40] = 1;
            const u64 v[3] = {1, 1920, 1080};
            std::memcpy(info + 0x48, v, sizeof(v));
            r.WriteOut(info, sizeof(info));
            r.Push<u64>(1);
        });
        Reg(1010, "OpenDisplay", [](Request& r) { r.Push<u64>(1); });
        Reg(1011, "OpenDefaultDisplay", [](Request& r) { r.Push<u64>(1); });
        Ok(1020, "CloseDisplay");
        Ok(1101, "SetDisplayEnabled");
        Reg(1102, "GetDisplayResolution", [](Request& r) {
            const bool docked = core::Sys().Cfg().docked;
            r.Push<s64>(docked ? 1920 : 1280);
            r.Push<s64>(docked ? 1080 : 720);
        });
        Reg(2020, "OpenLayer", [](Request& r) {
            const auto p = NativeWindowParcel(1);
            r.Push<u64>(r.WriteOut(p));
        });
        Ok(2021, "CloseLayer");
        Reg(2030, "CreateStrayLayer", [](Request& r) {
            const auto p = NativeWindowParcel(1);
            r.Push<u64>(1);
            r.Push<u64>(r.WriteOut(p));
        });
        Ok(2031, "DestroyStrayLayer");
        Ok(2101, "SetLayerScalingMode");
        Reg(2102, "ConvertScalingMode", [](Request& r) { r.Push<u64>(r.ArgAt<u32>(0)); });
        Reg(5202, "GetDisplayVsyncEvent", [](Request& r) { r.PushCopy(VsyncEvent()); });
        Reg(5203, "GetDisplayVsyncEventForDebug", [](Request& r) { r.PushCopy(VsyncEvent()); });
    }
};

class ViRoot final : public Service {
public:
    explicit ViRoot(std::string n) : Service(std::move(n)) {
        for (u32 id : {0u, 1u, 2u, 3u})
            Reg(id, "GetDisplayService", [](Request& r) { r.PushInterface(Make<IApplicationDisplayService>()); });
    }
};

} // namespace

void RegisterVi(ServiceManager& sm) {
    for (const char* n : {"vi:m", "vi:u", "vi:s"})
        sm.RegisterService(n, [n] { return Make<ViRoot>(n); });
}

} // namespace hle
