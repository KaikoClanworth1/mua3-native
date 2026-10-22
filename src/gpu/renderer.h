#pragma once
#include "common/common.h"

namespace gpu {

struct PresentInfo;

// Host graphics backend. Called on the GPU thread; reads engine state from gpu::G().M3D().
class Renderer {
public:
    virtual ~Renderer() = default;
    // CLEAR_BUFFERS: `mask` bit0 depth, bit1 stencil, bits 2-5 RGBA; `rt` = render target index.
    virtual void Clear(u32 rt, u32 mask) = 0;
    virtual void Draw() = 0;
    virtual void Present(const PresentInfo& p) = 0;
    // Guest-visible memory was written by the GPU (I2M, DMA, 2D); drop cached copies overlapping it.
    virtual void InvalidateRegion(u64 gpu_va, u64 size) {}
    // 2D engine surface copy (Fermi 0x902D); returns false if unsupported.
    virtual bool Blit2D(const u32* regs) { return false; }
    // Called at the end of each command submission.
    virtual void FlushCommands() {}
};

} // namespace gpu
