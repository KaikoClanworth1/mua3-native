#include <deque>
#include <thread>

#include <windows.h>
#include <xaudio2.h>

#include <SDL3/SDL.h>

#include "platform/platform.h"

#pragma comment(lib, "xaudio2.lib")
#pragma comment(lib, "ole32.lib")

namespace platform {

namespace {
constexpr u32 kRate = 48000;
constexpr u32 kChannels = 2;
constexpr u32 kChunkFrames = 240; // 5 ms
} // namespace

struct AudioOut::Impl {
    // Shared ring of interleaved s16 samples.
    mutable std::mutex mutex;
    std::deque<s16> queue;
    float volume = 1.0f;
    std::atomic<bool> running{false};
    std::thread feeder;

    // XAudio2
    IXAudio2* xa = nullptr;
    IXAudio2MasteringVoice* master = nullptr;
    IXAudio2SourceVoice* voice = nullptr;
    std::vector<std::vector<s16>> ring = std::vector<std::vector<s16>>(8);
    size_t ring_pos = 0;

    // SDL
    SDL_AudioStream* stream = nullptr;

    std::vector<s16> Take(size_t frames) {
        std::vector<s16> out(frames * kChannels, 0);
        std::lock_guard lk(mutex);
        const size_t n = std::min(out.size(), queue.size());
        for (size_t i = 0; i < n; i++)
            out[i] = s16(std::clamp(int(queue[i] * volume), -32768, 32767));
        queue.erase(queue.begin(), queue.begin() + n);
        return out;
    }

    ~Impl() {
        running = false;
        if (feeder.joinable())
            feeder.join();
        if (voice)
            voice->DestroyVoice();
        if (master)
            master->DestroyVoice();
        if (xa)
            xa->Release();
        if (stream)
            SDL_DestroyAudioStream(stream);
    }
};

static AudioOut g_audio;
AudioOut& Audio() { return g_audio; }

bool AudioOut::Init(core::AudioBackend backend, int volume) {
    backend_ = backend;
    impl_ = std::make_shared<Impl>();
    impl_->volume = std::clamp(volume, 0, 100) / 100.0f;
    if (backend == core::AudioBackend::Null)
        return true;

    if (backend == core::AudioBackend::XAudio2) {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        WAVEFORMATEX wf{};
        wf.wFormatTag = WAVE_FORMAT_PCM;
        wf.nChannels = kChannels;
        wf.nSamplesPerSec = kRate;
        wf.wBitsPerSample = 16;
        wf.nBlockAlign = kChannels * 2;
        wf.nAvgBytesPerSec = kRate * wf.nBlockAlign;
        if (FAILED(XAudio2Create(&impl_->xa, 0, XAUDIO2_DEFAULT_PROCESSOR)) ||
            FAILED(impl_->xa->CreateMasteringVoice(&impl_->master)) ||
            FAILED(impl_->xa->CreateSourceVoice(&impl_->voice, &wf))) {
            LOG_ERROR("Audio", "XAudio2 init failed; falling back to SDL");
            backend_ = core::AudioBackend::SDL;
        } else {
            impl_->voice->Start();
            impl_->running = true;
            auto* im = impl_.get();
            impl_->feeder = std::thread([im] {
                SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
                while (im->running) {
                    XAUDIO2_VOICE_STATE st{};
                    im->voice->GetState(&st, XAUDIO2_VOICE_NOSAMPLESPLAYED);
                    if (st.BuffersQueued < 4) {
                        auto& buf = im->ring[im->ring_pos++ % im->ring.size()];
                        buf = im->Take(kChunkFrames);
                        XAUDIO2_BUFFER xb{};
                        xb.AudioBytes = UINT32(buf.size() * 2);
                        xb.pAudioData = reinterpret_cast<const BYTE*>(buf.data());
                        im->voice->SubmitSourceBuffer(&xb);
                    } else {
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    }
                }
            });
            LOG_INFO("Audio", "XAudio2 output: {} Hz stereo", kRate);
            return true;
        }
    }

    SDL_AudioSpec spec{SDL_AUDIO_S16LE, int(kChannels), int(kRate)};
    impl_->stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (!impl_->stream) {
        LOG_ERROR("Audio", "SDL audio init failed: {}", SDL_GetError());
        backend_ = core::AudioBackend::Null;
        return false;
    }
    SDL_ResumeAudioStreamDevice(impl_->stream);
    impl_->running = true;
    auto* im = impl_.get();
    impl_->feeder = std::thread([im] {
        while (im->running) {
            if (SDL_GetAudioStreamQueued(im->stream) < int(kChunkFrames * kChannels * 2 * 4)) {
                auto buf = im->Take(kChunkFrames);
                SDL_PutAudioStreamData(im->stream, buf.data(), int(buf.size() * 2));
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
    });
    LOG_INFO("Audio", "SDL audio output: {} Hz stereo", kRate);
    return true;
}

void AudioOut::Shutdown() { impl_.reset(); }

void AudioOut::SetVolume(int volume) {
    if (!impl_)
        return;
    std::lock_guard lk(impl_->mutex);
    impl_->volume = std::clamp(volume, 0, 100) / 100.0f;
}

void AudioOut::Push(const s16* samples, size_t frames) {
    if (!impl_)
        return;
    std::lock_guard lk(impl_->mutex);
    // Keep latency bounded (~100 ms).
    const size_t max = kRate / 10 * kChannels;
    impl_->queue.insert(impl_->queue.end(), samples, samples + frames * kChannels);
    if (impl_->queue.size() > max)
        impl_->queue.erase(impl_->queue.begin(), impl_->queue.begin() + (impl_->queue.size() - max));
}

size_t AudioOut::Queued() const {
    if (!impl_)
        return 0;
    std::lock_guard lk(impl_->mutex);
    return impl_->queue.size() / kChannels;
}

std::string AudioOut::BackendName() const {
    switch (backend_) {
    case core::AudioBackend::XAudio2: return "XAudio2";
    case core::AudioBackend::SDL: return "SDL3";
    default: return "None";
    }
}

} // namespace platform
