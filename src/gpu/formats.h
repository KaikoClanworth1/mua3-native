#pragma once
#include <dxgiformat.h>

#include "common/common.h"

namespace gpu {

// Maxwell render-target / zeta formats -> DXGI.
struct RtFormatInfo {
    DXGI_FORMAT dxgi = DXGI_FORMAT_UNKNOWN;
    u32 bpp = 4; // bytes per pixel
    bool depth = false;
    bool stencil = false;
};
RtFormatInfo RtFormat(u32 maxwell_rt_format);
RtFormatInfo ZetaFormat(u32 maxwell_zeta_format);

// Texture image control (TIC) formats.
struct TexFormatInfo {
    DXGI_FORMAT dxgi = DXGI_FORMAT_UNKNOWN;
    u32 block_w = 1, block_h = 1; // compressed block size in texels
    u32 bytes_per_block = 4;
    bool astc = false;       // needs CPU decode to RGBA8
    bool depth = false;
};
// `format` = component layout (bits 0-6), `types` = r/g/b/a component types, `srgb` from the TIC.
TexFormatInfo TicFormat(u32 format, u32 r_type, bool srgb);

// Block-linear (GOB 64x8) -> linear. `bpb` = bytes per element (texel or compressed block).
// width/height in elements. block_height/depth are log2 in GOBs.
void Deswizzle(const u8* src, u8* dst, u32 width, u32 height, u32 depth, u32 bpb, u32 block_height_log2,
               u32 block_depth_log2, u32 dst_pitch);
u64 BlockLinearSize(u32 width, u32 height, u32 depth, u32 bpb, u32 block_height_log2, u32 block_depth_log2);

// Decodes an ASTC 2D texture (bw x bh blocks) to RGBA8.
void DecodeAstc(const u8* src, u8* dst, u32 width, u32 height, u32 bw, u32 bh);

} // namespace gpu
