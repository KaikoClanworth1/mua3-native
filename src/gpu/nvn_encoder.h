#pragma once
// Host NVN encoder: applies the game's recorded NVN command stream (hle::nvn replay sink) to a DrawState
// (gpu/draw_state.h) and has the D3D11 renderer draw from it, without the driver's command buffers.

namespace gpu {

class Gpu;

// Registers the encoder as the recorder's sink. Call once, before the game renders.
void InstallNvnEncoder(Gpu& gpu);

} // namespace gpu
