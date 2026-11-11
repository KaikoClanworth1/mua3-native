// hid: controllers via the HID shared memory block (6.x layout).
#include <atomic>
#include <thread>

#include "core/hle/hid.h"
#include "core/hle/service.h"

namespace hle {

namespace hid {

namespace {

constexpr u64 kShmSize = 0x40000;
constexpr u64 kNpadBase = 0x9A00;
constexpr u64 kNpadSize = 0x5000;
constexpr u64 kLifoSize = 0x350;
constexpr int kLifoEntries = 17;

std::mutex g_pad_mutex;
// Player 1 is always present (a Pro Controller), so the game never asks for the controller applet.
PadInput g_pads[8] = {PadInput{true}};
std::shared_ptr<kernel::SharedMemory> g_shm;
std::shared_ptr<kernel::Event> g_style_event;
std::atomic<bool> g_running{false};
std::atomic<u32> g_supported_styles{0x1F};
u64 g_sampling = 0;

std::shared_ptr<kernel::SharedMemory> Shm() {
    if (!g_shm)
        g_shm = std::make_shared<kernel::SharedMemory>(kShmSize, "hid");
    return g_shm;
}

void WriteLifo(u8* lifo, u64 ts, const void* state, size_t state_size) {
    u64* hdr = reinterpret_cast<u64*>(lifo);
    hdr[0] = ts;
    hdr[1] = kLifoEntries;
    const u64 tail = (hdr[2] + 1) % kLifoEntries;
    u8* entry = lifo + 0x20 + tail * (state_size + 8);
    std::memcpy(entry, &g_sampling, 8);
    std::memcpy(entry + 8, state, state_size);
    hdr[2] = tail;
    hdr[3] = std::min<u64>(hdr[3] + 1, kLifoEntries - 1);
}

void UpdateOnce() {
    u8* base = Shm()->Data();
    const u64 ts = kernel::K().GetTicks();
    g_sampling++;
    PadInput pads[8];
    {
        std::lock_guard lk(g_pad_mutex);
        std::copy(std::begin(g_pads), std::end(g_pads), pads);
    }
    // Player 1 also drives the handheld slot (index 8) so handheld-only code paths see input: its buttons only.
    // MUA3 builds player 1's input from No.1 and, when Handheld is connected, from Handheld too, and ADDS the two
    // (main+0x175f80 -> 0x1773c0: buttons OR-ed, stick axes summed). Mirrored sticks reached the game doubled (2.0 at
    // full tilt), which through the camera's squared response turned it ~5x too fast. So the handheld sticks stay
    // centred.
    for (int idx = 0; idx < 10; idx++) {
        const PadInput& in = idx < 8 ? pads[idx] : pads[0];
        u8* np = base + kNpadBase + idx * kNpadSize;
        const bool handheld = idx == 8;
        if (idx == 9)
            continue;
        const u32 style = !in.connected ? 0 : handheld ? 0x2 : 0x1; // Handheld / FullKey (Pro Controller)
        std::memcpy(np + 0x0, &style, 4);
        const u32 assignment = 0; // dual
        std::memcpy(np + 0x4, &assignment, 4);
        // Full key colors: attribute ok, main/sub
        const u32 fullkey[3] = {0, 0xFF2D2D2D, 0xFFE6E6E6};
        std::memcpy(np + 0x8, fullkey, 12);
        const u32 joy[5] = {0, 0xFF0AB9E6, 0xFF001E1E, 0xFFFF3C28, 0xFF1E0A0A};
        std::memcpy(np + 0x14, joy, 20);

        struct {
            u64 sampling;
            u64 buttons;
            s32 lx, ly, rx, ry;
            u32 attributes;
            u32 reserved;
        } st{g_sampling, in.connected ? in.buttons : 0,
             handheld ? 0 : in.lx, handheld ? 0 : in.ly, handheld ? 0 : in.rx, handheld ? 0 : in.ry,
             in.connected ? (handheld ? 0x3Fu : 0x1u) : 0u, 0};
        for (int l = 0; l < 7; l++)
            WriteLifo(np + 0x28 + l * kLifoSize, ts, &st, sizeof(st));
        const u32 device_type = !in.connected ? 0 : handheld ? 0x4 : 0x1;
        std::memcpy(np + 0x4188, &device_type, 4);
        const u64 sys_props = in.connected ? 0x1F : 0;
        std::memcpy(np + 0x4190, &sys_props, 8);
        const u32 battery[3] = {4, 4, 4};
        std::memcpy(np + 0x419C, battery, 12);
    }
}

} // namespace

void SetPad(int player, const PadInput& in) {
    if (player < 0 || player >= 8)
        return;
    std::lock_guard lk(g_pad_mutex);
    g_pads[player] = in;
    if (player == 0)
        g_pads[0].connected = true;
}

void StartUpdateThread() {
    if (g_running.exchange(true))
        return;
    std::thread([] {
        while (g_running) {
            UpdateOnce();
            std::this_thread::sleep_for(std::chrono::milliseconds(4));
        }
    }).detach();
}

} // namespace hid

namespace {

class IAppletResource final : public Service {
public:
    IAppletResource() : Service("hid:IAppletResource") {
        Reg(0, "GetSharedMemoryHandle", [](Request& r) {
            r.PushCopy(hid::Shm());
            hid::StartUpdateThread();
        });
    }
};

class IActiveVibrationDeviceList final : public Service {
public:
    IActiveVibrationDeviceList() : Service("hid:IActiveVibrationDeviceList") { Ok(0, "ActivateVibrationDevice"); }
};

class HidService final : public Service {
public:
    HidService() : Service("hid") {
        Reg(0, "CreateAppletResource", [](Request& r) { r.PushInterface(Make<IAppletResource>()); });
        for (u32 id : {1u, 11u, 21u, 31u, 40u, 41u, 51u, 55u, 56u, 58u, 59u, 60u, 61u, 62u, 63u, 64u, 65u, 66u, 67u,
                       68u, 69u, 70u, 71u, 72u, 73u, 74u, 75u, 76u, 77u, 78u, 79u, 80u, 81u, 82u, 87u, 88u, 89u,
                       91u, 103u, 104u, 120u, 122u, 123u, 124u, 125u, 126u, 128u, 129u, 130u, 131u, 132u, 133u,
                       134u, 201u, 206u, 208u, 209u, 210u, 211u, 300u, 301u, 302u, 303u, 304u, 305u, 400u, 401u,
                       402u, 403u, 404u, 405u, 406u, 407u, 408u, 409u, 500u, 501u, 502u, 503u, 504u, 505u, 510u,
                       511u, 512u, 513u, 520u, 521u, 1000u, 1001u})
            Ok(id, "Stub");
        Reg(100, "SetSupportedNpadStyleSet", [](Request& r) {
            hid::g_supported_styles = r.ArgAt<u32>(0);
            LOG_INFO("hid", "SetSupportedNpadStyleSet {:#x}", r.ArgAt<u32>(0));
        });
        Reg(101, "GetSupportedNpadStyleSet", [](Request& r) { r.Push<u32>(hid::g_supported_styles); });
        Ok(102, "SetSupportedNpadIdType");
        Reg(106, "AcquireNpadStyleSetUpdateEventHandle", [](Request& r) {
            if (!hid::g_style_event)
                hid::g_style_event = MakeEvent("hid:style", true);
            r.PushCopy(hid::g_style_event);
        });
        Ok(107, "DisconnectNpad");
        Reg(108, "GetPlayerLedPattern", [](Request& r) { r.Push<u64>(1); });
        Ok(109, "ActivateNpadWithRevision");
        Reg(121, "GetNpadJoyHoldType", [](Request& r) { r.Push<u64>(1); });
        Reg(200, "GetVibrationDeviceInfo", [](Request& r) {
            r.Push<u32>(1); // linear resonant actuator
            r.Push<u32>(0);
        });
        Reg(202, "GetActualVibrationValue", [](Request& r) {
            const f32 v[4] = {0, 160.0f, 0, 320.0f};
            r.PushRaw(v, sizeof(v));
        });
        Reg(203, "CreateActiveVibrationDeviceList",
            [](Request& r) { r.PushInterface(Make<IActiveVibrationDeviceList>()); });
        Reg(207, "SendVibrationGcErmCommand", [](Request&) {});
        Reg(82, "IsSixAxisSensorAtRest", [](Request& r) { r.Push<u8>(1); });
        Reg(66, "StartSixAxisSensor", [](Request&) {});
        Reg(67, "StopSixAxisSensor", [](Request&) {});
        Reg(68, "IsSixAxisSensorFusionEnabled", [](Request& r) { r.Push<u8>(0); });
        Reg(71, "GetSixAxisSensorFusionParameters", [](Request& r) {
            r.Push<f32>(0.03f);
            r.Push<f32>(0.4f);
        });
        Reg(80, "GetGyroscopeZeroDriftMode", [](Request& r) { r.Push<u32>(1); });
        Reg(1000, "SetNpadCommunicationMode", [](Request&) {});
        Reg(1001, "GetNpadCommunicationMode", [](Request& r) { r.Push<u32>(0); });
        quiet_ = true;
    }
};

} // namespace

void RegisterHid(ServiceManager& sm) {
    sm.RegisterService("hid", [] { return Make<HidService>(); });
    sm.RegisterService("irs", [] { return MakeStub("irs"); });
    sm.RegisterService("hid:sys", [] { return MakeStub("hid:sys"); });
}

} // namespace hle
