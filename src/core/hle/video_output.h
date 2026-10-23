#pragma once
#include <functional>

#include "common/common.h"

// Movie playback without NVDEC emulation: the game streams an .mp4 from the RomFS and feeds its H.264 to NVDEC,
// then asks VIC to convert each decoded picture into a surface it renders. Here the same file is decoded on the
// host (Media Foundation) and every VIC execute writes the next frame into the VIC output surface.
namespace video {

struct VicState {
    u64 config = 0; // device addresses (IOVA) set by the VIC command stream
    u64 luma = 0, chroma = 0;
};

// One VIC Execute. `translate` maps an IOVA to a guest CPU address (0 if unmapped).
void VicExecute(const VicState& s, const std::function<u64(u64)>& translate);

// The RomFS read `offset` bytes into a movie file: a read at the start (re)starts that movie.
void OnMovieRead(const fs::path& host_path, u64 offset);

} // namespace video
