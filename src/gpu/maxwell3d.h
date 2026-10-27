#pragma once
#include <array>
#include <functional>
#include <vector>

#include "common/common.h"

namespace gpu {

class Gpu;

// Maxwell B (0xB197) 3D engine register offsets (in 32-bit words).
namespace reg3d {
constexpr u32 kMacroUploadAddr = 0x45, kMacroUploadData = 0x46, kMacroEntry = 0x47, kMacroBind = 0x48;
constexpr u32 kI2MLineLength = 0x60, kI2MLineCount = 0x61, kI2MDstHigh = 0x62, kI2MDstLow = 0x63, kI2MPitch = 0x64;
constexpr u32 kI2MExec = 0x6C, kI2MData = 0x6D;
constexpr u32 kSyncInfo = 0xB2;
constexpr u32 kRenderTarget = 0x200;   // [8] x 0x10
constexpr u32 kViewportTransform = 0x280; // [16] x 8: scale xyz, translate xyz, swizzle, snap
constexpr u32 kViewport = 0x300;       // [16] x 4: x(u16 x, u16 w), y, near, far
constexpr u32 kVertexBufferFirst = 0x35D, kVertexBufferCount = 0x35E;
constexpr u32 kClearColor = 0x360, kClearDepth = 0x364, kClearStencil = 0x368;
constexpr u32 kZetaAddrHigh = 0x3F8, kZetaAddrLow = 0x3F9, kZetaFormat = 0x3FA, kZetaTileMode = 0x3FB;
constexpr u32 kVertexAttribFormat = 0x458; // [32]
constexpr u32 kRtControl = 0x487;
constexpr u32 kZetaWidth = 0x48A, kZetaHeight = 0x48B, kZetaDepth = 0x48C;
constexpr u32 kDepthTestEnable = 0x4B3, kDepthWriteEnable = 0x4BA, kAlphaTestEnable = 0x4BB;
constexpr u32 kDepthTestFunc = 0x4C3, kAlphaTestRef = 0x4C4, kAlphaTestFunc = 0x4C5;
constexpr u32 kBlendColor = 0x4C7;
constexpr u32 kBlendSeparateAlpha = 0x4CF, kBlendEqRgb = 0x4D0, kBlendSrcRgb = 0x4D1, kBlendDstRgb = 0x4D2;
constexpr u32 kBlendEqA = 0x4D3, kBlendSrcA = 0x4D4, kBlendDstA = 0x4D6, kBlendEnableCommon = 0x4D7;
constexpr u32 kBlendEnable = 0x4D8; // [8]
constexpr u32 kStencilEnable = 0x4E0, kStencilFrontOpFail = 0x4E1, kStencilFrontOpZFail = 0x4E2;
constexpr u32 kStencilFrontOpZPass = 0x4E3, kStencilFrontFunc = 0x4E4, kStencilFrontRef = 0x4E5;
constexpr u32 kStencilFrontFuncMask = 0x4E6, kStencilFrontMask = 0x4E7;
constexpr u32 kScreenYControl = 0x4EB;
constexpr u32 kZetaEnable = 0x54E;
constexpr u32 kTscAddrHigh = 0x557, kTscAddrLow = 0x558, kTscLimit = 0x559;
constexpr u32 kTicAddrHigh = 0x55D, kTicAddrLow = 0x55E, kTicLimit = 0x55F;
constexpr u32 kStencilTwoSide = 0x565, kStencilBackOpFail = 0x566, kStencilBackOpZFail = 0x567;
constexpr u32 kStencilBackOpZPass = 0x568, kStencilBackFunc = 0x569;
constexpr u32 kCodeAddrHigh = 0x582, kCodeAddrLow = 0x583;
constexpr u32 kDrawEnd = 0x585, kDrawBegin = 0x586;
constexpr u32 kIndexAddrHigh = 0x5F2, kIndexAddrLow = 0x5F3, kIndexLimitHigh = 0x5F4, kIndexLimitLow = 0x5F5;
constexpr u32 kIndexFormat = 0x5F6, kIndexFirst = 0x5F7, kIndexCount = 0x5F8;
constexpr u32 kCullEnable = 0x646, kFrontFace = 0x647, kCullFace = 0x648;
constexpr u32 kPolygonOffsetFillEnable = 0x372, kSlopeScaleDepthBias = 0x55B, kDepthBias = 0x56F, kDepthBiasClamp = 0x61F;
constexpr u32 kViewportTransformEnable = 0x64B;
constexpr u32 kClearBuffers = 0x674;
constexpr u32 kColorMask = 0x680; // [8]
constexpr u32 kQueryAddrHigh = 0x6C0, kQueryAddrLow = 0x6C1, kQuerySequence = 0x6C2, kQueryGet = 0x6C3;
constexpr u32 kVertexArray = 0x700; // [32] x 4: stride|enable, addr hi, addr lo, divisor
constexpr u32 kIndependentBlend = 0x780; // [8] x 8
constexpr u32 kVertexArrayLimit = 0x7C0; // [32] x 2
constexpr u32 kShaderConfig = 0x800;     // [6] x 0x10: enable|program, offset
constexpr u32 kCbSize = 0x8E0, kCbAddrHigh = 0x8E1, kCbAddrLow = 0x8E2, kCbPos = 0x8E3, kCbData = 0x8E4;
constexpr u32 kCbBind = 0x904; // [5] x 8
constexpr u32 kTexCbIndex = 0x982;
constexpr u32 kMacroCallBase = 0xE00;
} // namespace reg3d

enum class ShaderStage { Vertex = 0, TessControl, TessEval, Geometry, Fragment, Compute };

struct ConstBufferBinding {
    bool enabled = false;
    u64 address = 0;
    u32 size = 0;
};

class Maxwell3D {
public:
    explicit Maxwell3D(Gpu& gpu);

    void CallMethod(u32 method, u32 arg);
    void CallMacro(u32 index, const std::vector<u32>& params);

    u32 Reg(u32 r) const { return regs_[r]; }
    float RegF(u32 r) const {
        float f;
        std::memcpy(&f, &regs_[r], 4);
        return f;
    }
    u64 Addr(u32 hi_reg) const { return (u64(regs_[hi_reg] & 0xFF) << 32) | regs_[hi_reg + 1]; }

    // Bound constant buffers per graphics stage (0..4).
    const ConstBufferBinding& CBuf(u32 stage, u32 index) const { return cbufs_[stage][index]; }

    // Host NVN encoder (src/gpu/nvn_encoder.cpp). `intercept` sees every draw before it is rendered and returns true
    // if it took care of it (the native replay draws it from its own register file); `suppress_clears` drops
    // CLEAR_BUFFERS (the replay clears instead). With `capture` set, draws only report their state, and clears and
    // constant buffer uploads are ignored (a register file filled by the encoder, compared against the driver's).
    std::function<bool(const Maxwell3D&)> intercept;
    std::function<void(const Maxwell3D&)> capture;
    bool suppress_clears = false;
    const std::array<u32, 0x1000>& Regs() const { return regs_; }
    const ConstBufferBinding (&CBufs() const)[5][18] { return cbufs_; }

    // State of the current draw, for the renderer.
    struct DrawState {
        u32 topology = 0;
        bool indexed = false;
        u32 first = 0, count = 0;
        u32 instance = 0;
        u64 indirect_args = 0; // non-zero: the arguments live here (compute output), count/first unknown
    } draw;

private:
    void ProcessDrawEnd();
    void ProcessClear(u32 arg);
    void ProcessCbData(u32 arg);
    void ProcessCbBind(u32 stage, u32 arg);
    void ProcessQuery(u32 arg);
    void ProcessI2M(u32 method, u32 arg);
    void ExecuteMacro(u32 start, const std::vector<u32>& params);

    Gpu& gpu_;
    std::array<u32, 0x1000> regs_{};
    std::array<u32, 0x2000> macro_code_{};
    std::array<u32, 0x80> macro_entry_{};
    ConstBufferBinding cbufs_[5][18];
    bool index_count_written_ = false;
    u32 instance_counter_ = 0;
    u32 i2m_cursor_ = 0;
};

} // namespace gpu
