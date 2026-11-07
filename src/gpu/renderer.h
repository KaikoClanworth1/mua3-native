#pragma once
#include "common/common.h"

namespace gpu {

struct PresentInfo;
struct ComputeLaunch;

// Host graphics backend. Called on the GPU thread; draws from gpu::G().State().
class Renderer {
public:
    virtual ~Renderer() = default;
    virtual void ClearColor(u32 rt, const float color[4]) = 0;
    virtual void ClearDepthStencil(bool depth, bool stencil, float depth_value, u8 stencil_value) = 0;
    virtual void Draw() = 0;
    virtual void Present(const PresentInfo& p) = 0;
    // Guest-visible memory was written by the GPU (I2M, DMA, 2D); drop cached copies overlapping it.
    virtual void InvalidateRegion(u64 gpu_va, u64 size) {}
    // 2D engine surface copy (Fermi 0x902D); returns false if unsupported.
    virtual bool Blit2D(const u32* regs) { return false; }
    virtual void Dispatch(const ComputeLaunch& l) {}
    // Lands the results of earlier GPU work (compute) that overlap [gpu_va, +size) in guest memory.
    virtual void SyncRegion(u64 gpu_va, u64 size) {}
    // Index count of the culling list the host wrote at `index_addr` (MUA3's indirect draws read it back from the
    // GPU; the host keeps every triangle, so it already knows it), 0 if unknown.
    virtual u32 CullListCount(u64 index_addr) { return 0; }
    // Called at the end of each command submission.
    virtual void FlushCommands() {}
    // Called after Present, outside any renderer lock: blocks while too many frames are still queued on the host GPU.
    virtual void ThrottleFrames() {}
};

} // namespace gpu
