#pragma once
#include <functional>

#include "common/common.h"

namespace hle {

// Receives the audio renderer's final mix: interleaved stereo s16 at 48 kHz (set by the frontend). `queued`
// reports frames still buffered for the device; the renderer paces its updates to keep that topped up.
void SetAudioOutput(std::function<void(const s16* stereo, size_t frames)> fn, std::function<size_t()> queued);

} // namespace hle
