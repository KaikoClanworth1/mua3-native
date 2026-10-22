// audren:u / audout:u. The renderer protocol is answered so the game's sound library runs;
// mixing to the host device (XAudio2/SDL) hooks in at RequestUpdate.
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
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
        if (rate)
            rate_ = rate;
        if (samples)
            samples_ = samples;
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
        // Voices: run each voice's wave-buffer queue at its sample rate so the game's buffer
        // bookkeeping (release, stop and flush waits) behaves like the DSP.
        const size_t voices_at = pools_at + mempools_in + voice_res_in;
        for (u32 i = 0; i < n_voices; i++) {
            u8 e[0x10]{};
            const size_t v = voices_at + i * 0x170;
            if (v + 0x170 <= in.size()) {
                VoiceState& vs = voices_[i % kMaxVoices];
                UpdateVoice(vs, &in[v]);
                std::memcpy(e, &vs.played, 8);
                std::memcpy(e + 8, &vs.consumed, 4);
            }
            out.insert(out.end(), e, e + 0x10);
        }
        // Effects: EffectInfo::InParameter is 0xC0 bytes (+0 type, +1 is_new, +2 enabled). The server's
        // StoreStatus reports 3 (enabled) or 4 (disabled); IsAuxRemovable waits for the latter before the
        // game may tear an effect (and the voice routed through it) down.
        const size_t effects_at = voices_at + voices_in;
        for (u32 i = 0; i < n_effects; i++) {
            u8 e[0x10]{};
            const size_t fx = effects_at + size_t(i) * 0xC0;
            if (fx + 0xC0 <= in.size() && in[fx] != 0)
                e[0] = in[fx + 2] ? 3 : 4;
            out.insert(out.end(), e, e + 0x10);
        }
        out.insert(out.end(), size_t(n_sinks) * 0x20, 0);
        out.insert(out.end(), 0x10, 0); // performance
        out.insert(out.end(), 0xB0, 0); // behavior
        std::vector<u8> render_info(0x10);
        if (++elapsed_frames_ % 2000 == 0) {
            u32 queued = 0;
            for (u32 i = 0; i < std::min(n_voices, kMaxVoices); i++)
                for (auto& b : voices_[i].buffers)
                    queued += b.queued;
            LOG_INFO("audren", "{} updates ({} voices, {} queued wave buffers)", elapsed_frames_, n_voices, queued);
        }
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

    // Server-side view of one voice (the DSP's wave-buffer ring).
    struct VoiceState {
        struct Buffer {
            bool queued = false;
            bool loop = false;
            s64 remaining = 0; // samples left to play
        } buffers[4];
        u32 cursor = 0;   // ring slot being played
        u64 played = 0;   // samples played since the last stop
        u32 consumed = 0; // total wave buffers released back to the game
        double frac = 0;
        bool stopped = true; // a fresh voice starts out stopped
        u8 last_state = 0xFF;
        bool last_in_use = false;
    };
    static constexpr u32 kMaxVoices = 1024;

    // VoiceInfo::InParameter (0x170 bytes): +0x08 is_new, +0x09 in_use, +0x0A play state
    // (0 started, 1 stopped, 2 paused), +0x0C sample rate, +0x1C pitch, +0x60 wave buffers [4] x 0x38
    // (+0x10 start offset, +0x14 end offset, +0x18 loop, +0x1A sent to DSP), +0x159 flush count.
    void UpdateVoice(VoiceState& vs, const u8* p) {
        auto rd32 = [&](size_t o) { return common::Read<u32>(p + o); };
        const bool is_new = p[0x08] != 0, in_use = p[0x09] != 0;
        const u8 play_state = p[0x0A];
        static const bool trace = std::getenv("MUA3_TRACE_VOICES") != nullptr;
        if (trace && (is_new || play_state != vs.last_state || in_use != vs.last_in_use || p[0x159]))
            LOG_INFO("audren", "voice {} new {} in_use {} state {} -> {} rate {} flush {} consumed {} played {}",
                     common::Read<u32>(p), is_new, in_use, vs.last_state, play_state, common::Read<u32>(p + 0x0C),
                     p[0x159], vs.consumed, vs.played);
        if (is_new)
            vs = VoiceState{};
        vs.last_state = play_state;
        vs.last_in_use = in_use;
        if (!in_use)
            return;
        // New wave buffers (sent_to_DSP == 0) always (re)initialise their ring slot, like the server does.
        for (u32 k = 0; k < 4; k++) {
            const u8* wb = p + 0x60 + k * 0x38;
            if (wb[0x1A] || common::Read<u64>(wb) == 0)
                continue; // owned by the DSP already, or an empty slot
            auto& b = vs.buffers[k];
            const s32 start = common::Read<s32>(wb + 0x10), end = common::Read<s32>(wb + 0x14);
            b.queued = true;
            b.loop = wb[0x18] != 0;
            b.remaining = std::max<s64>(end - start, 1);
        }
        auto release_front = [&] {
            vs.buffers[vs.cursor] = {};
            vs.consumed++;
            vs.cursor = (vs.cursor + 1) % 4;
        };
        // Flush: the server counts every flushed slot as consumed, valid or not.
        for (u32 n = p[0x159]; n > 0; n--)
            release_front();
        if (play_state == 1) { // RequestStop -> Stopped: the valid buffers ahead of the play cursor are returned
            if (!vs.stopped) {
                for (u32 k = 0; k < 4 && vs.buffers[vs.cursor].queued; k++)
                    release_front();
                vs.played = 0;
                vs.frac = 0;
                vs.stopped = true;
            }
            return;
        }
        vs.stopped = false;
        if (play_state != 0 || !started_)
            return;
        float pitch;
        std::memcpy(&pitch, p + 0x1C, 4);
        if (!(pitch > 0.0f && pitch < 16.0f))
            pitch = 1.0f;
        const double want = double(rd32(0x0C) ? rd32(0x0C) : rate_) * samples_ / rate_ * pitch + vs.frac;
        s64 todo = s64(want);
        vs.frac = want - double(todo);
        for (u32 guard = 0; todo > 0 && guard < 8; guard++) {
            auto& b = vs.buffers[vs.cursor];
            if (!b.queued)
                break;
            const s64 n = std::min(todo, b.remaining);
            b.remaining -= n;
            todo -= n;
            vs.played += u64(n);
            if (b.remaining > 0)
                break;
            if (b.loop) {
                b.remaining = 1; // looping buffers are never released; keep them alive
                break;
            }
            release_front();
        }
    }

    u32 rate_ = 48000, samples_ = 240;
    bool started_ = false;
    u64 elapsed_frames_ = 0;
    VoiceState voices_[kMaxVoices];
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
