// audren:u / audout:u. The renderer protocol is answered so the game's sound library runs;
// mixing to the host device (XAudio2/SDL) hooks in at RequestUpdate.
#include <chrono>
#include <thread>

#include "core/hle/service.h"

namespace hle {

namespace {

std::shared_ptr<kernel::Event> AudioTickEvent() {
    static std::shared_ptr<kernel::Event> e = [] {
        auto ev = MakeEvent("audren:system");
        std::thread([ev] {
            for (;;) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                kernel::K().SignalEvent(*ev);
            }
        }).detach();
        return ev;
    }();
    return e;
}

class IAudioRenderer final : public Service {
public:
    IAudioRenderer(u32 rate, u32 samples, u32 mix_buffers) : Service("audren:IAudioRenderer") {
        quiet_ = true;
        Reg(0, "GetSampleRate", [rate](Request& r) { r.Push<u32>(rate); });
        Reg(1, "GetSampleCount", [samples](Request& r) { r.Push<u32>(samples); });
        Reg(2, "GetMixBufferCount", [mix_buffers](Request& r) { r.Push<u32>(mix_buffers); });
        Reg(3, "GetState", [this](Request& r) { r.Push<u32>(started_ ? 0 : 1); });
        auto update = [this](Request& r) { RequestUpdate(r); };
        Reg(4, "RequestUpdate", update);
        Reg(10, "RequestUpdateAuto", update);
        Reg(5, "Start", [this](Request&) { started_ = true; });
        Reg(6, "Stop", [this](Request&) { started_ = false; });
        Reg(7, "QuerySystemEvent", [](Request& r) { r.PushCopy(AudioTickEvent()); });
        Ok(8, "SetRenderingTimeLimit");
        Reg(9, "GetRenderingTimeLimit", [](Request& r) { r.Push<u32>(100); });
    }

private:
    void RequestUpdate(Request& r) {
        auto in = r.ReadIn(0);
        if (in.size() < 0x40)
            return;
        auto rd = [&](size_t o) { return common::Read<u32>(&in[o]); };
        const u32 revision = rd(0);
        const u32 behavior_in = rd(4), mempools_in = rd(8), voices_in = rd(0xC), voice_res_in = rd(0x10);
        const u32 effects_in = rd(0x14), mixes_in = rd(0x18), sinks_in = rd(0x1C);
        const u32 n_pools = mempools_in / 0x20, n_voices = voices_in / 0x170, n_effects = effects_in / 0xC0;
        const u32 n_sinks = sinks_in / 0x140;

        std::vector<u8> out(0x40);
        auto wr = [&](size_t o, u32 v) { std::memcpy(&out[o], &v, 4); };
        // Memory pools: acknowledge attach/detach requests
        size_t pools_at = 0x40 + behavior_in;
        for (u32 i = 0; i < n_pools; i++) {
            u32 state = pools_at + i * 0x20 + 0x14 <= in.size() ? rd(pools_at + i * 0x20 + 0x10) : 0;
            if (state == 4)
                state = 5; // RequestAttach -> Attached
            else if (state == 2)
                state = 3; // RequestDetach -> Detached
            u8 e[0x10]{};
            std::memcpy(e, &state, 4);
            out.insert(out.end(), e, e + 0x10);
        }
        // Voices: report wave buffers as consumed so streaming keeps moving.
        const size_t voices_at = pools_at + mempools_in + voice_res_in;
        for (u32 i = 0; i < n_voices; i++) {
            u8 e[0x10]{};
            const size_t v = voices_at + i * 0x170;
            if (v + 0x170 <= in.size()) {
                const bool in_use = in[v + 4] != 0;
                if (in_use) {
                    played_[i % 1024] += 240;
                    const u64 played = played_[i % 1024];
                    std::memcpy(e, &played, 8);
                    const u32 consumed = 1;
                    std::memcpy(e + 8, &consumed, 4);
                }
            }
            out.insert(out.end(), e, e + 0x10);
        }
        out.insert(out.end(), size_t(n_effects) * 0x10, 0);
        out.insert(out.end(), size_t(n_sinks) * 0x20, 0);
        out.insert(out.end(), 0x10, 0); // performance
        out.insert(out.end(), 0xB0, 0); // behavior
        std::vector<u8> render_info(0x10);
        elapsed_frames_++;
        std::memcpy(render_info.data(), &elapsed_frames_, 8);
        out.insert(out.end(), render_info.begin(), render_info.end());

        wr(0x00, revision);
        wr(0x04, 0xB0);
        wr(0x08, n_pools * 0x10);
        wr(0x0C, n_voices * 0x10);
        wr(0x14, n_effects * 0x10);
        wr(0x1C, n_sinks * 0x20);
        wr(0x20, 0x10);
        wr(0x28, 0x10);
        wr(0x3C, u32(out.size()));
        (void)mixes_in;
        r.WriteOut(out, 0);
        if (r.OutBuffer(1).size) {
            std::vector<u8> perf(size_t(r.OutBuffer(1).size));
            r.WriteOut(perf, 1);
        }
    }

    bool started_ = false;
    u64 elapsed_frames_ = 0;
    u64 played_[1024]{};
};

class IAudioDevice final : public Service {
public:
    IAudioDevice() : Service("audren:IAudioDevice") {
        auto list = [](Request& r) {
            char names[2][0x100]{};
            std::strcpy(names[0], "AudioTvOutput");
            std::strcpy(names[1], "AudioStereoJackOutput");
            r.Push<u32>(u32(r.WriteOut(names, sizeof(names)) / 0x100));
        };
        Reg(0, "ListAudioDeviceName", list);
        Reg(6, "ListAudioDeviceNameAuto", list);
        Ok(1, "SetAudioDeviceOutputVolume");
        Ok(7, "SetAudioDeviceOutputVolumeAuto");
        Reg(2, "GetAudioDeviceOutputVolume", [](Request& r) { r.Push<f32>(1.0f); });
        Reg(8, "GetAudioDeviceOutputVolumeAuto", [](Request& r) { r.Push<f32>(1.0f); });
        auto active = [](Request& r) {
            char name[0x100] = "AudioTvOutput";
            r.WriteOut(name, sizeof(name));
        };
        Reg(3, "GetActiveAudioDeviceName", active);
        Reg(10, "GetActiveAudioDeviceNameAuto", active);
        Reg(4, "QueryAudioDeviceSystemEvent", [](Request& r) { r.PushCopy(MakeEvent("auddev", true)); });
        Reg(5, "GetActiveChannelCount", [](Request& r) { r.Push<u32>(2); });
        Reg(11, "QueryAudioDeviceInputEvent", [](Request& r) { r.PushCopy(MakeEvent("auddevin")); });
        Reg(12, "QueryAudioDeviceOutputEvent", [](Request& r) { r.PushCopy(MakeEvent("auddevout")); });
    }
};

class AudRen final : public Service {
public:
    AudRen() : Service("audren:u") {
        Reg(0, "OpenAudioRenderer", [](Request& r) {
            const u32 rate = r.ArgAt<u32>(0), samples = r.ArgAt<u32>(4), mix = r.ArgAt<u32>(8);
            LOG_INFO("audio", "OpenAudioRenderer rate={} samples={} mix={} voices={} sinks={} effects={} rev={:#x}",
                     rate, samples, mix, r.ArgAt<u32>(16), r.ArgAt<u32>(20), r.ArgAt<u32>(24), r.ArgAt<u32>(0x30));
            r.PushInterface(Make<IAudioRenderer>(rate, samples, mix));
        });
        Reg(1, "GetWorkBufferSize", [](Request& r) {
            const u32 voices = r.ArgAt<u32>(16), effects = r.ArgAt<u32>(24), mix = r.ArgAt<u32>(8);
            const u32 samples = r.ArgAt<u32>(4);
            const u64 size = 0x4000 + u64(voices) * 0x1000 + u64(effects) * 0x2000 + u64(mix) * samples * 4 * 4 +
                             0x100000;
            r.Push<u64>(common::AlignUp(size, 0x1000));
        });
        Reg(2, "GetAudioDeviceService", [](Request& r) { r.PushInterface(Make<IAudioDevice>()); });
        Reg(4, "GetAudioDeviceServiceWithRevisionInfo", [](Request& r) { r.PushInterface(Make<IAudioDevice>()); });
    }
};

class IAudioOut final : public Service {
public:
    IAudioOut() : Service("audout:IAudioOut") {
        Reg(0, "GetAudioOutState", [](Request& r) { r.Push<u32>(0); });
        Ok(1, "Start");
        Ok(2, "Stop");
        auto append = [this](Request& r) {
            std::lock_guard lk(mutex_);
            released_.push_back(r.ArgAt<u64>(0));
            kernel::K().SignalEvent(*event_);
        };
        Reg(3, "AppendAudioOutBuffer", append);
        Reg(7, "AppendAudioOutBufferAuto", append);
        Reg(4, "RegisterBufferEvent", [this](Request& r) { r.PushCopy(event_); });
        auto get = [this](Request& r) {
            std::lock_guard lk(mutex_);
            const size_t max = size_t(r.OutSize() / 8);
            const size_t n = std::min(max, released_.size());
            r.WriteOut(released_.data(), n * 8);
            released_.erase(released_.begin(), released_.begin() + n);
            r.Push<u32>(u32(n));
        };
        Reg(5, "GetReleasedAudioOutBuffer", get);
        Reg(8, "GetReleasedAudioOutBufferAuto", get);
        Reg(6, "ContainsAudioOutBuffer", [](Request& r) { r.Push<u8>(0); });
    }

private:
    std::mutex mutex_;
    std::vector<u64> released_;
    std::shared_ptr<kernel::Event> event_ = MakeEvent("audout");
};

class AudOut final : public Service {
public:
    AudOut() : Service("audout:u") {
        auto list = [](Request& r) {
            char name[0x100] = "DeviceOut";
            r.WriteOut(name, sizeof(name));
            r.Push<u32>(1);
        };
        Reg(0, "ListAudioOuts", list);
        Reg(2, "ListAudioOutsAuto", list);
        auto open = [](Request& r) {
            r.Push<u32>(48000);
            r.Push<u32>(2);
            r.Push<u32>(2); // PCM16
            r.Push<u32>(0);
            char name[0x100] = "DeviceOut";
            r.WriteOut(name, sizeof(name));
            r.PushInterface(Make<IAudioOut>());
        };
        Reg(1, "OpenAudioOut", open);
        Reg(3, "OpenAudioOutAuto", open);
    }
};

} // namespace

void RegisterAudio(ServiceManager& sm) {
    sm.RegisterService("audren:u", [] { return Make<AudRen>(); });
    sm.RegisterService("audout:u", [] { return Make<AudOut>(); });
    sm.RegisterService("audin:u", [] { return MakeStub("audin:u"); });
    sm.RegisterService("audctl", [] { return MakeStub("audctl"); });
}

} // namespace hle
