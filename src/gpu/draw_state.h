#pragma once
#include "common/common.h"

namespace gpu {

// What the game's NVN commands have set: the encoder (gpu/nvn_encoder.cpp) writes it and the renderer draws from it.

// NVN shader stages.
enum NvnStage : u32 { kStageVertex = 0, kStageFragment = 1, kStageGeometry = 2, kStageTessControl = 3, kStageTessEval = 4, kNumGraphicsStages = 5 };

// Constant buffer tables are indexed by the stage numbers compiled shaders and the driver's tables use.
constexpr u32 kCbVertex = 0, kCbFragment = 4, kCbGeometry = 3;
constexpr u32 kTextureHandleCb = 2; // the driver's constant buffer holding texture handles

struct ConstBufferBinding {
    bool enabled = false;
    u64 address = 0;
    u32 size = 0;
};

struct ColorTargetState {
    u64 addr = 0;
    u32 width = 0, height = 0, format = 0, tile = 0; // format 0: no target
};

struct DepthTargetState {
    bool enabled = false;
    u64 addr = 0;
    u32 width = 0, height = 0, format = 0;
};

struct BlendTargetState {
    u32 eq_rgb = 0, src_rgb = 0, dst_rgb = 0, eq_a = 0, src_a = 0, dst_a = 0;
};

struct StencilFaceState {
    u32 func = 0, op_fail = 0, op_zfail = 0, op_zpass = 0;
};

struct VertexStreamState {
    bool enabled = false;
    u64 addr = 0, limit = 0; // limit: last byte
    u32 stride = 0, divisor = 0;
};

// The draw being issued.
struct DrawCall {
    u32 topology = 0;
    bool indexed = false;
    u32 first = 0, count = 0;
    u32 instance = 0;
    u64 indirect_args = 0; // non-zero: the arguments live here (compute output), count/first unknown
};

struct DrawState {
    u64 program[kNumGraphicsStages]{}; // Maxwell program header address per NVN stage, 0: none

    u32 rt_count = 1;
    ColorTargetState color[8];
    DepthTargetState depth;

    float vp_scale[3]{}, vp_translate[3]{}; // viewport 0
    float depth_near = 0, depth_far = 0;
    bool scissor_enable = false;
    u32 scissor[4]{}; // left, top, right, bottom

    bool blend_enable[8]{};
    BlendTargetState blend[8];
    float blend_color[4]{};
    u32 color_mask[8] = {0xF, 0xF, 0xF, 0xF, 0xF, 0xF, 0xF, 0xF}; // bits r, g, b, a

    bool depth_test = false, depth_write = false;
    u32 depth_func = 0;
    bool stencil_test = false;
    u32 stencil_ref = 0, stencil_read_mask = 0, stencil_write_mask = 0;
    StencilFaceState stencil_front, stencil_back;

    bool alpha_test = false;
    u32 alpha_func = 0, alpha_ref = 0;

    bool cull_enable = false, front_ccw = false;
    u32 cull_face = 0; // 0x404 front, 0x405 back, 0x408 both
    bool polygon_offset = false;
    float depth_bias = 0, slope_scale_depth_bias = 0, depth_bias_clamp = 0;

    u32 attrib[32]{}; // stream bits 0-4, constant bit 6, offset bits 7-20, size bits 21-26, type bits 27-29
    VertexStreamState streams[16];

    u64 index_addr = 0;
    u32 index_format = 0; // 0 u8, 1 u16, 2 u32
    s32 base_vertex = 0;
    u32 base_instance = 0;

    u64 tic_base = 0, tsc_base = 0; // texture and sampler pools
    ConstBufferBinding cb[5][18];

    DrawCall draw;
};

// One compute dispatch.
struct ComputeLaunch {
    u64 program = 0;
    u32 program_offset = 0; // within its 64 KB (debug switches name programs by it)
    u32 grid[3]{}, block[3]{};
    u32 shared_bytes = 0;
    u64 cb_addr[8]{};
    u32 cb_size[8]{}; // 0: not bound
    u64 tic_base = 0, tsc_base = 0;
};

} // namespace gpu
