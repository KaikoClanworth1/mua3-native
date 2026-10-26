#pragma once
// Host NVN encoder: turns the game's recorded NVN command stream (hle::nvn replay sink) into writes to a Maxwell3D
// register file of its own, the same state the NVN driver would have produced, so the D3D11 renderer can draw it
// without the driver's command buffers and the GPU emulation. See docs/native_renderer_plan.md.
//
// MUA3_NVN_VERIFY=<seconds> (needs MUA3_NVN_RECORD=1): after that time, encodes one frame into a capture-only
// register file, takes a snapshot at every draw, and compares it with the driver path's snapshots of the same submit
// (Maxwell3D::observe), writing the per-register differences to nvn_verify.txt.

namespace gpu {

class Gpu;

// Registers the encoder as the replay sink when MUA3_NVN_VERIFY is set. Call once, before the game renders.
void InstallNvnEncoder(Gpu& gpu);

} // namespace gpu
