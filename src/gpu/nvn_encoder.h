#pragma once
// Host NVN encoder: turns the game's recorded NVN command stream (hle::nvn replay sink) into writes to a Maxwell3D
// register file of its own, the same state the NVN driver would have produced, so the D3D11 renderer can draw it
// without the driver's command buffers. See docs/native_renderer_plan.md.

namespace gpu {

class Gpu;

// Registers the encoder as the recorder's sink. Call once, before the game renders.
void InstallNvnEncoder(Gpu& gpu);

} // namespace gpu
