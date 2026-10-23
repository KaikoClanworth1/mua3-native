// audren:u / audout:u. The renderer protocol is answered so the game's sound library runs, and each update mixes
// the voices (PCM / DSP-ADPCM wave buffers) through the game's submixes into the final mix, which goes to the
// host device (XAudio2/SDL) via SetAudioOutput.
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <set>
#include <thread>
#include <unordered_map>
#include <vector>

#include "core/hle/audio_output.h"
#include "core/hle/service.h"
#include "core/memory.h"
#include "core/system.h"

namespace hle {

namespace {
std::function<void(const s16*, size_t)> g_audio_output;
std::function<size_t()> g_audio_queued;
std::atomic<s64> g_last_output_ns{0}; // steady clock time of the last mix sent to the device
s64 NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
} // namespace

void SetAudioOutput(std::function<void(const s16* stereo, size_t frames)> fn, std::function<size_t()> queued) {
    g_audio_output = std::move(fn);
    g_audio_queued = std::move(queued);
}

namespace {

std::shared_ptr<kernel::Event> AudioTickEvent() {
    static std::shared_ptr<kernel::Event> e = [] {
        auto ev = MakeEvent("audren:system");
        // The game renders one audio frame (5 ms) per signal. While the mix reaches the device, signal whenever
        // its queue runs low so production follows the device clock; otherwise tick every 5 ms.
        std::thread([ev] {
            for (;;) {
                const bool flowing = g_audio_queued && NowNs() - g_last_output_ns.load() < 100'000'000;
                if (!flowing) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                    kernel::K().SignalEvent(*ev);
                    continue;
                }
                if (g_audio_queued() < 48000 * 40 / 1000) {
                    kernel::K().SignalEvent(*ev);
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                } else {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
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
        // Voice channel resources (0x70 each: id, mix volumes[24], in use): per-channel send levels.
        const size_t res_at = pools_at + mempools_in;
        channel_volumes_.clear();
        for (size_t o = res_at; o + 0x70 <= res_at + voice_res_in && o + 0x70 <= in.size(); o += 0x70) {
            if (!in[o + 0x64])
                continue;
            auto& v = channel_volumes_[rd(o)];
            std::memcpy(v.data(), &in[o + 4], 24 * 4);
        }
        // Mixes (0x930 each): give every mix in use its range of mix buffers (final mix first).
        const size_t effects_end = pools_at + mempools_in + voice_res_in + voices_in + effects_in;
        const size_t sinks_at = in.size() >= 0x40 ? 0x40 + behavior_in + mempools_in + voice_res_in + voices_in + effects_in +
                                                        mixes_in + SplitterSize(in, effects_end)
                                                  : 0;
        const size_t mixes_at = sinks_at - mixes_in;
        SetupMixes(in, mixes_at, mixes_in / 0x930);
        static bool layout_logged = false;
        if (!layout_logged && n_voices && !mix_order_.empty()) {
            layout_logged = true;
            LOG_INFO("audren", "update layout: in {:#x} behavior {:#x} pools {:#x} channel res {:#x} voices {:#x} effects {:#x} "
                     "mixes {:#x} sinks {:#x} perf {:#x}; {} mixes in use, {} channel resources, {} splitters, {} destinations",
                     in.size(), behavior_in, mempools_in, voice_res_in, voices_in, effects_in, mixes_in, sinks_in, rd(0x20),
                     mix_order_.size(), channel_volumes_.size(), splitters_.size(), destinations_.size());
        }

        // Voices: run each voice's wave-buffer queue at its sample rate so the game's buffer
        // bookkeeping (release, stop and flush waits) behaves like the DSP, mixing what it plays.
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
        ParseEffects(in, effects_at, n_effects);
        if (static const bool dbg = std::getenv("MUA3_AUDIO_DEBUG") != nullptr; dbg) {
            static std::set<std::string> seen;
            for (u32 i = 0; i < n_effects; i++) {
                const size_t fx = effects_at + size_t(i) * 0xC0;
                if (fx + 0xC0 > in.size() || in[fx] == 0)
                    continue;
                const std::string d = fmt::format("effect {} type {} enabled {} mix {} params {}", i, in[fx], in[fx + 2],
                                                  common::Read<s32>(&in[fx + 4]), common::ToHex(std::span(&in[fx + 0x20], 0x40)));
                if (seen.insert(d).second && seen.size() < 64)
                    LOG_INFO("audren", "{}", d);
            }
        }
        for (u32 i = 0; i < n_effects; i++) {
            u8 e[0x10]{};
            const size_t fx = effects_at + size_t(i) * 0xC0;
            if (fx + 0xC0 <= in.size() && in[fx] != 0)
                e[0] = in[fx + 2] ? 3 : 4;
            out.insert(out.end(), e, e + 0x10);
        }
        FinishMix(in, sinks_at, n_sinks);
        out.insert(out.end(), size_t(n_sinks) * 0x20, 0);
        out.insert(out.end(), 0x10, 0); // performance
        out.insert(out.end(), 0xB0, 0); // behavior
        std::vector<u8> render_info(0x10);
        if (++elapsed_frames_ % 2000 == 0) {
            u32 queued = 0;
            for (u32 i = 0; i < std::min(n_voices, kMaxVoices); i++)
                for (auto& b : voices_[i].buffers)
                    queued += b.queued;
            float mix_peak = 0;
            for (float v : mix_)
                mix_peak = std::max(mix_peak, std::abs(v));
            LOG_INFO("audren", "{} updates ({} voices, {} queued wave buffers) | mixing: {} playing, formats {:#x}, source peak {:.3f}, "
                     "{} routed / {} unrouted channels, {} mixes, last mix peak {:.3f}", elapsed_frames_, n_voices, queued,
                     stat_playing_, stat_format_mask_, stat_src_peak_, stat_routed_, stat_unrouted_, mix_order_.size(), mix_peak);
            if (std::getenv("MUA3_AUDIO_DEBUG")) {
                std::string a;
                for (auto& [k, v] : aux_stats_)
                    a += fmt::format(" [mix {} ch {}: send {:.3f} ret {:.3f}]", k / 32, k % 32, v.first, v.second);
                LOG_INFO("audren", "aux peaks:{}", a);
                std::string sp;
                for (size_t m : mix_order_) {
                    const u32 split = common::Read<u32>(&in[m + 0x928]);
                    auto it = splitters_.find(split);
                    if (it == splitters_.end())
                        continue;
                    sp += fmt::format(" [mix {} splitter {}:", common::Read<u32>(&in[m + 0x10]), split);
                    for (u32 d : it->second)
                        if (auto dd = destinations_.find(d); dd != destinations_.end())
                            sp += fmt::format(" d{}->mix{}{} vol {:.2f},{:.2f},{:.2f}", d, dd->second.mix_id,
                                              dd->second.in_use ? "" : "(off)", dd->second.volume[0], dd->second.volume[1],
                                              dd->second.volume[2]);
                    sp += "]";
                }
                LOG_INFO("audren", "mix splitters:{}", sp);
                aux_stats_.clear();
            }
            stat_playing_ = stat_routed_ = stat_unrouted_ = 0;
            stat_format_mask_ = 0;
            stat_src_peak_ = 0;
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
            s64 length = 0;    // end - start
            s64 start = 0;     // first sample (frame) in the wave buffer
            u64 address = 0, context = 0;
        } buffers[4];
        u32 cursor = 0;   // ring slot being played
        u64 played = 0;   // samples played since the last stop
        s16 hist1 = 0, hist2 = 0; // DSP-ADPCM decoder history
        float last[6]{};          // last source frame of the previous update (resampler continuity)
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
            b.length = b.remaining;
            b.start = start;
            b.address = common::Read<u64>(wb);
            b.context = common::Read<u64>(wb + 0x20);
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
        const s64 total = todo;
        const u32 channels = std::clamp<u32>(rd32(0x18), 1, 6);
        const u8 format = p[0x0B];
        std::vector<float>& src = mix_scratch_;
        src.assign(size_t(total) * channels, 0.0f);
        s64 got = 0;
        for (u32 guard = 0; todo > 0 && guard < 8; guard++) {
            auto& b = vs.buffers[vs.cursor];
            if (!b.queued)
                break;
            const s64 n = std::min(todo, b.remaining);
            DecodeSamples(vs, b, p, format, channels, b.length - b.remaining, n, &src[size_t(got) * channels]);
            got += n;
            b.remaining -= n;
            todo -= n;
            vs.played += u64(n);
            if (b.remaining > 0)
                break;
            if (b.loop) {
                b.remaining = b.length; // play the buffer again from its start
                continue;
            }
            release_front();
        }
        stat_playing_++;
        stat_format_mask_ |= 1u << (format & 31);
        { // MUA3_AUDIO_DEBUG: describe each distinct kind of playing voice once
            static const bool dbg = std::getenv("MUA3_AUDIO_DEBUG") != nullptr;
            static std::set<u64> seen;
            const u32 mix = common::Read<u32>(p + 0x58), split = common::Read<u32>(p + 0x5C);
            const u64 sig = u64(format) | u64(channels) << 8 | u64(mix & 0xFFFF) << 16 | u64(split & 0xFFFF) << 32 |
                            u64(rd32(0x0C) / 1000) << 48;
            if (dbg && seen.insert(sig).second) {
                std::string dests;
                if (auto it = splitters_.find(split); it != splitters_.end())
                    for (u32 d : it->second)
                        if (auto dd = destinations_.find(d); dd != destinations_.end())
                            dests += fmt::format(" d{}->mix{}{}", d, dd->second.mix_id, dd->second.in_use ? "" : "(off)");
                float vol;
                std::memcpy(&vol, p + 0x20, 4);
                LOG_INFO("audren", "voice kind: id {} format {} ch {} rate {} vol {:.2f} mix {} splitter {}{}", common::Read<u32>(p),
                         format, channels, rd32(0x0C), vol, s32(mix), s32(split), dests);
            }
        }
        for (s64 i = 0; i < got * s64(channels); i += 7)
            stat_src_peak_ = std::max(stat_src_peak_, std::abs(src[size_t(i)]));
        MixVoice(vs, p, channels, src.data(), total);
    }

    // Decodes `count` frames starting `pos` frames into wave buffer `b` (interleaved floats, `channels` wide).
    static void DecodeSamples(VoiceState& vs, const VoiceState::Buffer& b, const u8* p, u8 format, u32 channels, s64 pos,
                              s64 count, float* out) {
        auto& mem = core::Sys().Mem();
        if (!b.address)
            return;
        const s64 first = b.start + pos;
        switch (format) {
        case 1: // PCM s8
        case 2: // PCM s16
        case 5: { // float
            const u32 bytes = format == 1 ? 1 : format == 2 ? 2 : 4;
            const u64 a = b.address + u64(first) * channels * bytes;
            const size_t n = size_t(count) * channels;
            if (!mem.IsValid(a, n * bytes))
                return;
            const u8* src = mem.Ptr<u8>(a);
            for (size_t i = 0; i < n; i++) {
                if (format == 1)
                    out[i] = s8(src[i]) / 128.0f;
                else if (format == 2)
                    out[i] = common::Read<s16>(src + i * 2) / 32768.0f;
                else
                    out[i] = common::Read<float>(src + i * 4);
            }
            break;
        }
        case 6: { // Nintendo DSP-ADPCM (mono): 8-byte frames of 14 samples, coefficients at the voice's src data
            s16 coef[16]{};
            const u64 ca = common::Read<u64>(p + 0x48);
            if (ca && mem.IsValid(ca, 32))
                mem.ReadBlock(ca, coef, 32);
            if (pos == 0 && b.context && mem.IsValid(b.context, 6)) { // buffer start: history from its context
                vs.hist1 = mem.Read<s16>(b.context + 2);
                vs.hist2 = mem.Read<s16>(b.context + 4);
            }
            for (s64 i = 0; i < count; i++) {
                const s64 n = first + i;
                const u64 fa = b.address + u64(n / 14) * 8;
                if (!mem.IsValid(fa, 8))
                    break;
                const u8 header = mem.Read<u8>(fa);
                const u32 idx = u32(n % 14);
                const u8 byte = mem.Read<u8>(fa + 1 + idx / 2);
                int nib = (idx & 1) ? (byte & 0xF) : (byte >> 4);
                if (nib >= 8)
                    nib -= 16;
                const int ci = (header >> 4) & 7;
                const s32 v = ((nib * (1 << (header & 0xF))) << 11) + 1024 + coef[ci * 2] * vs.hist1 + coef[ci * 2 + 1] * vs.hist2;
                const s16 sample = s16(std::clamp(v >> 11, -32768, 32767));
                vs.hist2 = vs.hist1;
                vs.hist1 = sample;
                out[size_t(i) * channels] = sample / 32768.0f;
            }
            break;
        }
        default:
            break;
        }
    }

    // Resamples a voice's source frames to one output frame of samples_ and adds it to its mix's buffers through
    // the per-channel mix volumes (voice channel resources).
    void MixVoice(VoiceState& vs, const u8* p, u32 channels, const float* src, s64 count) {
        if (mix_.empty() || count <= 0)
            return;
        float volume;
        std::memcpy(&volume, p + 0x20, 4);
        if (!(volume > 0.0f))
            return;
        // Destination of each channel: the voice's mix with its channel resource volumes, or (splitter voices) the
        // channel's splitter destination with that destination's volumes.
        const u32 mix_id = common::Read<u32>(p + 0x58), splitter_id = common::Read<u32>(p + 0x5C);
        auto sit = splitter_id != 0xFFFFFFFF ? splitters_.find(splitter_id) : splitters_.end();
        for (u32 c = 0; c < channels; c++) {
            // Sends of this channel: its channel resource, or (splitter voices) every splitter destination at
            // index c, c + channels, c + 2 * channels... (how the DSP walks a splitter for multi-destination sends).
            struct Send {
                u32 mix;
                const float* volume;
            };
            Send sends[8];
            u32 nsend = 0;
            if (sit != splitters_.end()) {
                for (size_t k = c; k < sit->second.size() && nsend < 8; k += channels) {
                    auto dit = destinations_.find(sit->second[k]);
                    if (dit != destinations_.end() && dit->second.in_use)
                        sends[nsend++] = {dit->second.mix_id, dit->second.volume.data()};
                }
            } else if (auto rit = channel_volumes_.find(common::Read<u32>(p + 0x140 + c * 4)); rit != channel_volumes_.end()) {
                sends[nsend++] = {mix_id, rit->second.data()};
            }
            if (!nsend)
                continue;
            // Linear resampling: output i maps to source position (i + 1) * count / samples_ - 1 (previous
            // update's last frame at -1).
            std::vector<float>& res = resampled_;
            res.resize(samples_);
            for (u32 i = 0; i < samples_; i++) {
                const double pos = double(i + 1) * double(count) / double(samples_) - 1.0;
                const s64 i0 = s64(std::floor(pos));
                const float t = float(pos - double(i0));
                const float a = i0 < 0 ? vs.last[c] : src[size_t(i0) * channels + c];
                const float bb = i0 + 1 < count ? src[size_t(i0 + 1) * channels + c] : a;
                res[i] = (a + (bb - a) * t) * volume;
            }
            for (u32 k = 0; k < nsend; k++) {
                auto mit = mix_offset_.find(sends[k].mix);
                if (mit == mix_offset_.end()) {
                    stat_unrouted_++;
                    continue;
                }
                stat_routed_++;
                const u32 base = mit->second.first, nbuf = mit->second.second;
                for (u32 j = 0; j < nbuf && j < 24; j++) {
                    const float mv = sends[k].volume[j];
                    if (mv == 0.0f)
                        continue;
                    float* dst = &mix_[size_t(base + j) * samples_];
                    for (u32 i = 0; i < samples_; i++)
                        dst[i] += res[i] * mv;
                }
            }
            vs.last[c] = src[size_t(count - 1) * channels + c];
        }
    }

    // Splitter data (between effects and mixes) when present: "SNDH" header {magic, info count, data count, pad},
    // then infos "SNDI" {magic, id, sample rate, destination count, destination data ids..., padding} and destinations
    // "SNDD" {magic, id, mix volumes[24], mix id, in use, pad} (0x70). Returns its size; fills splitters_.
    // Effects. Only Aux (type 2) matters for MUA3: its sound engine mixes music, dialogue and movie audio on the
    // CPU. Per update the DSP writes each input mix buffer into the "send" ring and reads the "return" ring (which
    // the game filled) into the output buffer. Rings: AuxBufferInfo {cpu info, dsp info} (0x40 each) followed by
    // s32 samples, count_max per ring; channel i uses offset i * samples within the frame, and the last channel
    // advances the dsp write/read offset by channels * samples. Disabled aux copies input to output.
    struct Effect {
        u8 type = 0;
        bool enabled = false;
        s32 mix = -1, order = 0;
        s8 inputs[24]{}, outputs[24]{};
        u32 channels = 0, count_max = 0;
        u64 send_info = 0, send_data = 0, return_info = 0, return_data = 0;
    };
    std::vector<Effect> effects_;

    void ParseEffects(const std::vector<u8>& in, size_t at, u32 n) {
        effects_.clear();
        for (u32 i = 0; i < n; i++) {
            const size_t fx = at + size_t(i) * 0xC0;
            if (fx + 0xC0 > in.size() || in[fx] != 2)
                continue;
            Effect e;
            e.type = in[fx];
            e.enabled = in[fx + 2] != 0;
            e.mix = common::Read<s32>(&in[fx + 4]);
            e.order = common::Read<s32>(&in[fx + 0x18]);
            std::memcpy(e.inputs, &in[fx + 0x20], 24);
            std::memcpy(e.outputs, &in[fx + 0x38], 24);
            e.channels = std::min<u32>(common::Read<u32>(&in[fx + 0x50]), 24);
            e.count_max = common::Read<u32>(&in[fx + 0x58]);
            e.send_info = common::Read<u64>(&in[fx + 0x60]);
            e.send_data = common::Read<u64>(&in[fx + 0x68]);
            e.return_info = common::Read<u64>(&in[fx + 0x70]);
            e.return_data = common::Read<u64>(&in[fx + 0x78]);
            static int logged = 0;
            if (logged < 4 && e.send_info) {
                logged++;
                LOG_INFO("audren", "aux on mix {}: {} ch, count max {}, send info {:#x} data {:#x}, return info {:#x} data {:#x}",
                         e.mix, e.channels, e.count_max, e.send_info, e.send_data, e.return_info, e.return_data);
            }
            effects_.push_back(e);
        }
        std::stable_sort(effects_.begin(), effects_.end(), [](const Effect& a, const Effect& b) { return a.order < b.order; });
    }

    void ApplyEffects(u32 mix_id) {
        auto mit = mix_offset_.find(mix_id);
        if (mit == mix_offset_.end())
            return;
        auto& mem = core::Sys().Mem();
        const u32 base = mit->second.first, nbuf = mit->second.second;
        std::vector<s32>& tmp = aux_scratch_;
        tmp.resize(samples_);
        for (const Effect& e : effects_) {
            if (s32(mix_id) != e.mix)
                continue;
            for (u32 c = 0; c < e.channels; c++) {
                const s8 si = e.inputs[c], so = e.outputs[c];
                if (si < 0 || so < 0 || u32(si) >= nbuf || u32(so) >= nbuf)
                    continue;
                float* src = &mix_[size_t(base + u32(si)) * samples_];
                float* dst = &mix_[size_t(base + u32(so)) * samples_];
                const bool usable = e.enabled && e.send_info && e.return_info && e.send_data && e.return_data &&
                                    e.count_max >= samples_ && mem.IsValid(e.send_info, 0x80) && mem.IsValid(e.return_info, 0x80) &&
                                    mem.IsValid(e.send_data, u64(e.count_max) * 4) && mem.IsValid(e.return_data, u64(e.count_max) * 4);
                if (!usable) {
                    if (si != so)
                        std::memcpy(dst, src, samples_ * sizeof(float));
                    continue;
                }
                const bool last = c + 1 == e.channels;
                const u32 advance = last ? e.channels * samples_ : 0;
                // Send: this channel's frame at dsp write offset + c * samples.
                for (u32 i = 0; i < samples_; i++)
                    tmp[i] = s32(std::lround(std::clamp(src[i], -4.0f, 4.0f) * 32768.0f));
                RingTransfer(mem, e.send_info + 0x40, e.send_data, e.count_max, c * samples_, advance, true, tmp.data());
                // Return: what the game wrote for this channel.
                RingTransfer(mem, e.return_info + 0x40, e.return_data, e.count_max, c * samples_, advance, false, tmp.data());
                float sp = 0, rp = 0;
                for (u32 i = 0; i < samples_; i++) {
                    sp = std::max(sp, std::abs(src[i]));
                    dst[i] = float(tmp[i]) / 32768.0f;
                    rp = std::max(rp, std::abs(dst[i]));
                }
                auto& st = aux_stats_[u32(e.mix) * 32 + c];
                st.first = std::max(st.first, sp);
                st.second = std::max(st.second, rp);
            }
        }
    }

    // Copies samples_ s32 samples to (write) or from (read) a ring at `info`'s dsp offset (+0 read, +4 write) plus
    // `offset`, wrapping at count_max; then advances that offset by `advance`.
    void RingTransfer(core::Memory& mem, u64 info, u64 data, u32 count_max, u32 offset, u32 advance, bool write, s32* samples) {
        const u32 field = write ? 4 : 0;
        const u32 cur = mem.Read<u32>(info + field);
        u32 pos = (cur + offset) % count_max;
        for (u32 done = 0; done < samples_;) {
            const u32 n = std::min(count_max - pos, samples_ - done);
            if (write)
                mem.WriteBlock(data + u64(pos) * 4, samples + done, size_t(n) * 4);
            else
                mem.ReadBlock(data + u64(pos) * 4, samples + done, size_t(n) * 4);
            done += n;
            pos = (pos + n) % count_max;
        }
        if (advance)
            mem.Write<u32>(info + field, (cur + advance) % count_max);
    }

    // Only changed splitters/destinations are sent, so the tables persist across updates.
    size_t SplitterSize(const std::vector<u8>& in, size_t at) {
        if (at + 0x20 > in.size() || common::Read<u32>(&in[at]) != 0x48444E53)
            return 0;
        const s32 infos = common::Read<s32>(&in[at + 4]), datas = common::Read<s32>(&in[at + 8]);
        size_t o = at + 0x20;
        for (s32 i = 0; i < infos; i++) {
            if (o + 0x10 > in.size() || common::Read<u32>(&in[o]) != 0x49444E53) {
                SplitterParseError("info", i, o - at, in, o);
                return 0;
            }
            const s32 id = common::Read<s32>(&in[o + 4]);
            const u32 count = common::Read<u32>(&in[o + 12]);
            auto& dests = splitters_[u32(id)];
            dests.clear();
            for (u32 k = 0; k < count && o + 0x10 + k * 4 + 4 <= in.size(); k++)
                dests.push_back(common::Read<u32>(&in[o + 0x10 + k * 4]));
            // Entries carry padding after the destination ids (12 bytes seen with 4 destinations): resume at the next
            // entry's magic.
            o += 0x10 + size_t(count) * 4;
            for (u32 k = 0; k < 16 && o + 4 <= in.size(); k++, o += 4) {
                const u32 magic = common::Read<u32>(&in[o]);
                if (magic == 0x49444E53 || magic == 0x44444E53)
                    break;
            }
        }
        for (s32 i = 0; i < datas; i++) {
            if (o + 0x70 > in.size() || common::Read<u32>(&in[o]) != 0x44444E53) {
                SplitterParseError("destination", i, o - at, in, o);
                return 0;
            }
            Destination d;
            std::memcpy(d.volume.data(), &in[o + 8], 24 * 4);
            d.mix_id = common::Read<u32>(&in[o + 0x68]);
            d.in_use = in[o + 0x6C] != 0;
            if (std::getenv("MUA3_AUDIO_DEBUG")) {
                auto& old = destinations_[common::Read<u32>(&in[o + 4])];
                if (old.mix_id != d.mix_id || old.in_use != d.in_use)
                    LOG_INFO("audren", "splitter destination {} -> mix {} in use {} vol {:.2f},{:.2f}", common::Read<u32>(&in[o + 4]),
                             s32(d.mix_id), d.in_use, d.volume[0], d.volume[1]);
            }
            destinations_[common::Read<u32>(&in[o + 4])] = d;
            o += 0x70;
        }
        return o - at;
    }
    void SplitterParseError(const char* what, s32 index, size_t rel, const std::vector<u8>& in, size_t o) {
        static int n = 0;
        if (n++ < 8)
            LOG_WARN("audren", "splitter {} {} unparsed at +{:#x}: {}", what, index, rel,
                     common::ToHex(std::span(&in[std::min(o, in.size() - 0x20)], 0x20)));
    }
    struct Destination {
        u32 mix_id = 0;
        std::array<float, 24> volume{};
        bool in_use = false;
    };
    std::unordered_map<u32, std::vector<u32>> splitters_; // splitter id -> destination data ids
    std::unordered_map<u32, Destination> destinations_;

    void SetupMixes(const std::vector<u8>& in, size_t at, u32 n) {
        mix_offset_.clear();
        mix_order_.clear();
        u32 next = 0;
        std::vector<std::pair<s32, size_t>> mixes;
        for (u32 i = 0; i < n; i++) {
            const size_t m = at + size_t(i) * 0x930;
            if (m + 0x930 > in.size() || !in[m + 0x0C])
                continue;
            mixes.push_back({common::Read<s32>(&in[m + 0x10]), m});
        }
        std::sort(mixes.begin(), mixes.end()); // final mix (id 0) first
        for (auto& [id, m] : mixes) {
            const u32 count = std::min<u32>(common::Read<u32>(&in[m + 0x08]), 24);
            mix_offset_[u32(id)] = {next, count};
            mix_order_.push_back(m);
            next += count;
        }
        mix_.assign(size_t(next) * samples_, 0.0f);
    }

    // Submixes into their destinations (deepest first), then the final mix to the device sink. A mix sends either
    // to dest_mix_id (+0x924) through its volume matrix, or (dest unused) through splitter dest_splitter_id (+0x928):
    // source buffer a goes to that splitter's destination a (mix id + volumes per destination buffer).
    void FinishMix(const std::vector<u8>& in, size_t sinks_at, u32 n_sinks) {
        if (mix_.empty())
            return;
        std::unordered_map<u32, size_t> by_id;
        for (size_t m : mix_order_)
            by_id[common::Read<u32>(&in[m + 0x10])] = m;
        auto targets = [&](size_t m) {
            std::vector<u32> t;
            const u32 dest = common::Read<u32>(&in[m + 0x924]), split = common::Read<u32>(&in[m + 0x928]);
            if (by_id.count(dest))
                t.push_back(dest);
            else if (auto it = splitters_.find(split); it != splitters_.end())
                for (u32 d : it->second)
                    if (auto dit = destinations_.find(d); dit != destinations_.end() && dit->second.in_use)
                        t.push_back(dit->second.mix_id);
            return t;
        };
        std::unordered_map<u32, u32> depth; // longest path to a sink-side mix
        std::function<u32(u32, u32)> depth_of = [&](u32 id, u32 guard) -> u32 {
            if (auto it = depth.find(id); it != depth.end())
                return it->second;
            u32 d = 0;
            if (guard < 16 && id != 0)
                for (u32 t : targets(by_id[id]))
                    d = std::max(d, depth_of(t, guard + 1) + 1);
            depth[id] = d;
            return d;
        };
        std::vector<std::pair<u32, u32>> order;
        for (auto& [id, m] : by_id)
            if (id != 0)
                order.push_back({depth_of(id, 0), id});
        std::sort(order.rbegin(), order.rend());
        auto add = [&](u32 src_id, u32 a, u32 dst_id, u32 b, float gain) {
            auto s = mix_offset_.find(src_id), t = mix_offset_.find(dst_id);
            if (gain == 0.0f || s == mix_offset_.end() || t == mix_offset_.end() || a >= s->second.second || b >= t->second.second)
                return;
            const float* src = &mix_[size_t(s->second.first + a) * samples_];
            float* dst = &mix_[size_t(t->second.first + b) * samples_];
            for (u32 i = 0; i < samples_; i++)
                dst[i] += src[i] * gain;
        };
        for (auto& [d, id] : order) {
            ApplyEffects(id);
            const size_t m = by_id[id];
            const u32 nbuf = mix_offset_[id].second;
            float vol;
            std::memcpy(&vol, &in[m], 4);
            const u32 dest = common::Read<u32>(&in[m + 0x924]), split = common::Read<u32>(&in[m + 0x928]);
            if (by_id.count(dest)) {
                for (u32 a = 0; a < nbuf; a++)
                    for (u32 b = 0; b < 24; b++) {
                        float mv;
                        std::memcpy(&mv, &in[m + 0x24 + (size_t(a) * 24 + b) * 4], 4);
                        add(id, a, dest, b, mv * vol);
                    }
            } else if (auto it = splitters_.find(split); it != splitters_.end() && !it->second.empty()) {
                for (u32 a = 0; a < nbuf; a++)
                    for (size_t k = a; k < it->second.size(); k += nbuf) {
                        auto dit = destinations_.find(it->second[k]);
                        if (dit == destinations_.end() || !dit->second.in_use)
                            continue;
                        for (u32 b = 0; b < 24; b++)
                            add(id, a, dit->second.mix_id, b, dit->second.volume[b] * vol);
                    }
            }
        }
        ApplyEffects(0);
        // Device sink: input[] picks final mix buffers (FL, FR, FC, LFE, RL, RR); downmix to stereo.
        auto fin = mix_offset_.find(0);
        if (fin == mix_offset_.end() || !g_audio_output || !started_)
            return;
        s8 input[6] = {0, 1, -1, -1, -1, -1};
        u32 input_count = 2;
        for (u32 k = 0; k < n_sinks; k++) {
            const size_t sk = sinks_at + size_t(k) * 0x140;
            if (sk + 0x140 > in.size() || in[sk] != 1 || !in[sk + 1])
                continue;
            // SinkInfo: +0 type (1 device), +1 in use; device parameters at +0x20: name[0x100], input count, inputs[6].
            const u32 count = std::min<u32>(common::Read<u32>(&in[sk + 0x120]), 6);
            if (count) {
                input_count = count;
                std::memcpy(input, &in[sk + 0x124], 6);
            }
            break;
        }
        float fvol = 1.0f;
        for (size_t m : mix_order_)
            if (common::Read<s32>(&in[m + 0x10]) == 0)
                std::memcpy(&fvol, &in[m], 4);
        auto chan = [&](u32 k, u32 i) -> float {
            if (k >= input_count || input[k] < 0 || u32(input[k]) >= fin->second.second)
                return 0.0f;
            return mix_[size_t(fin->second.first + u32(input[k])) * samples_ + i];
        };
        static int graph_logged = 0;
        if (graph_logged < 2) {
            float peak = 0;
            for (float v : mix_)
                peak = std::max(peak, std::abs(v));
            if (peak > 0.05f) {
                graph_logged++;
                std::string g;
                for (size_t m : mix_order_) {
                    const u32 id = common::Read<u32>(&in[m + 0x10]);
                    float vol, pk = 0;
                    std::memcpy(&vol, &in[m], 4);
                    auto o = mix_offset_[id];
                    for (u32 k = 0; k < o.second * samples_; k++)
                        pk = std::max(pk, std::abs(mix_[size_t(o.first) * samples_ + k]));
                    g += fmt::format(" [mix {} -> {} vol {:.2f} bufs {}@{} peak {:.3f}]", id, s32(common::Read<u32>(&in[m + 0x924])), vol,
                                     o.second, o.first, pk);
                }
                LOG_INFO("audren", "mix graph:{} | sink inputs {} [{} {} {} {} {} {}] final vol {:.2f}", g, input_count, input[0], input[1],
                         input[2], input[3], input[4], input[5], fvol);
            }
        }
        std::vector<s16> pcm(size_t(samples_) * 2);
        for (u32 i = 0; i < samples_; i++) {
            float l = chan(0, i), r = chan(1, i);
            if (input_count >= 6) {
                const float c = chan(2, i) * 0.7071f;
                l += c + chan(4, i) * 0.7071f;
                r += c + chan(5, i) * 0.7071f;
            }
            pcm[i * 2] = s16(std::clamp(l * fvol * 32767.0f, -32768.0f, 32767.0f));
            pcm[i * 2 + 1] = s16(std::clamp(r * fvol * 32767.0f, -32768.0f, 32767.0f));
        }
        g_audio_output(pcm.data(), samples_);
        g_last_output_ns = NowNs();
        // MUA3_AUDIO_DUMP=<file.wav>: record the mix (debugging without listening).
        static FILE* dump = [] {
            const char* path = std::getenv("MUA3_AUDIO_DUMP");
            FILE* f = path ? std::fopen(path, "wb") : nullptr;
            if (f) {
                const u8 header[44] = {'R', 'I', 'F', 'F', 0xFF, 0xFF, 0xFF, 0x7F, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0, 2, 0,
                                       0x80, 0xBB, 0, 0, 0x00, 0xEE, 0x02, 0, 4, 0, 16, 0, 'd', 'a', 't', 'a', 0xFF, 0xFF, 0xFF, 0x7F};
                std::fwrite(header, 1, sizeof(header), f);
            }
            return f;
        }();
        if (dump) {
            std::fwrite(pcm.data(), 2, pcm.size(), dump);
            std::fflush(dump);
        }
    }

    std::unordered_map<u32, std::array<float, 24>> channel_volumes_; // voice channel resource id -> mix volumes
    std::unordered_map<u32, std::pair<u32, u32>> mix_offset_;        // mix id -> {first mix buffer, buffer count}
    std::vector<size_t> mix_order_;                                  // mixes in use (offsets into the update data)
    std::vector<float> mix_;                                         // mix buffers, samples_ each
    std::vector<float> mix_scratch_, resampled_;
    std::vector<s32> aux_scratch_;
    std::map<u32, std::pair<float, float>> aux_stats_; // mix * 32 + channel -> {send peak, return peak}
    u64 stat_playing_ = 0, stat_routed_ = 0, stat_unrouted_ = 0; // mixer diagnostics (periodic log)
    u32 stat_format_mask_ = 0;
    float stat_src_peak_ = 0;
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
