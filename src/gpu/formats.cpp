#include "gpu/formats.h"

namespace gpu {

RtFormatInfo RtFormat(u32 f) {
    switch (f) {
    case 0xC0: return {DXGI_FORMAT_R32G32B32A32_FLOAT, 16};
    case 0xC1: return {DXGI_FORMAT_R32G32B32A32_SINT, 16};
    case 0xC2: return {DXGI_FORMAT_R32G32B32A32_UINT, 16};
    case 0xC3: return {DXGI_FORMAT_R32G32B32A32_FLOAT, 16}; // RGBX32 float
    case 0xC6: return {DXGI_FORMAT_R16G16B16A16_UNORM, 8};
    case 0xC7: return {DXGI_FORMAT_R16G16B16A16_SNORM, 8};
    case 0xC8: return {DXGI_FORMAT_R16G16B16A16_SINT, 8};
    case 0xC9: return {DXGI_FORMAT_R16G16B16A16_UINT, 8};
    case 0xCA: return {DXGI_FORMAT_R16G16B16A16_FLOAT, 8};
    case 0xCB: return {DXGI_FORMAT_R32G32_FLOAT, 8};
    case 0xCC: return {DXGI_FORMAT_R32G32_SINT, 8};
    case 0xCD: return {DXGI_FORMAT_R32G32_UINT, 8};
    case 0xCE: return {DXGI_FORMAT_R16G16B16A16_FLOAT, 8}; // RGBX16 float
    case 0xCF: return {DXGI_FORMAT_B8G8R8A8_UNORM, 4};
    case 0xD0: return {DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, 4};
    case 0xD1: return {DXGI_FORMAT_R10G10B10A2_UNORM, 4};
    case 0xD2: return {DXGI_FORMAT_R10G10B10A2_UINT, 4};
    case 0xD5: return {DXGI_FORMAT_R8G8B8A8_UNORM, 4};
    case 0xD6: return {DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, 4};
    case 0xD7: return {DXGI_FORMAT_R8G8B8A8_SNORM, 4};
    case 0xD8: return {DXGI_FORMAT_R8G8B8A8_SINT, 4};
    case 0xD9: return {DXGI_FORMAT_R8G8B8A8_UINT, 4};
    case 0xDA: return {DXGI_FORMAT_R16G16_UNORM, 4};
    case 0xDB: return {DXGI_FORMAT_R16G16_SNORM, 4};
    case 0xDC: return {DXGI_FORMAT_R16G16_SINT, 4};
    case 0xDD: return {DXGI_FORMAT_R16G16_UINT, 4};
    case 0xDE: return {DXGI_FORMAT_R16G16_FLOAT, 4};
    case 0xDF: return {DXGI_FORMAT_R10G10B10A2_UNORM, 4}; // BGR10A2 (channel order approximated)
    case 0xE0: return {DXGI_FORMAT_R11G11B10_FLOAT, 4};
    case 0xE3: return {DXGI_FORMAT_R32_SINT, 4};
    case 0xE4: return {DXGI_FORMAT_R32_UINT, 4};
    case 0xE5: return {DXGI_FORMAT_R32_FLOAT, 4};
    case 0xE6: return {DXGI_FORMAT_B8G8R8X8_UNORM, 4};
    case 0xE7: return {DXGI_FORMAT_B8G8R8X8_UNORM_SRGB, 4};
    case 0xE8: return {DXGI_FORMAT_B5G6R5_UNORM, 2};
    case 0xE9: return {DXGI_FORMAT_B5G5R5A1_UNORM, 2};
    case 0xEA: return {DXGI_FORMAT_R8G8_UNORM, 2};
    case 0xEB: return {DXGI_FORMAT_R8G8_SNORM, 2};
    case 0xEC: return {DXGI_FORMAT_R8G8_SINT, 2};
    case 0xED: return {DXGI_FORMAT_R8G8_UINT, 2};
    case 0xEE: return {DXGI_FORMAT_R16_UNORM, 2};
    case 0xEF: return {DXGI_FORMAT_R16_SNORM, 2};
    case 0xF0: return {DXGI_FORMAT_R16_SINT, 2};
    case 0xF1: return {DXGI_FORMAT_R16_UINT, 2};
    case 0xF2: return {DXGI_FORMAT_R16_FLOAT, 2};
    case 0xF3: return {DXGI_FORMAT_R8_UNORM, 1};
    case 0xF4: return {DXGI_FORMAT_R8_SNORM, 1};
    case 0xF5: return {DXGI_FORMAT_R8_SINT, 1};
    case 0xF6: return {DXGI_FORMAT_R8_UINT, 1};
    case 0xF7: return {DXGI_FORMAT_A8_UNORM, 1};
    case 0xF8: return {DXGI_FORMAT_B5G5R5A1_UNORM, 2};
    case 0xF9: return {DXGI_FORMAT_R8G8B8A8_UNORM, 4};
    case 0xFA: return {DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, 4};
    default: return {DXGI_FORMAT_UNKNOWN, 4};
    }
}

RtFormatInfo ZetaFormat(u32 f) {
    switch (f) {
    case 0x0A: return {DXGI_FORMAT_D32_FLOAT, 4, true, false};
    case 0x13: return {DXGI_FORMAT_D16_UNORM, 2, true, false};
    case 0x14: return {DXGI_FORMAT_D24_UNORM_S8_UINT, 4, true, true}; // S8Z24
    case 0x15: return {DXGI_FORMAT_D24_UNORM_S8_UINT, 4, true, false}; // X8Z24
    case 0x16: return {DXGI_FORMAT_D24_UNORM_S8_UINT, 4, true, true}; // Z24S8
    case 0x19: return {DXGI_FORMAT_D32_FLOAT_S8X24_UINT, 8, true, true};
    default: return {DXGI_FORMAT_D24_UNORM_S8_UINT, 4, true, true};
    }
}

TexFormatInfo TicFormat(u32 format, u32 t, bool srgb) {
    // t: 1 snorm, 2 unorm, 3 sint, 4 uint, 5/6 normalized forced fp16, 7 float
    auto pick = [&](DXGI_FORMAT unorm, DXGI_FORMAT snorm, DXGI_FORMAT uint, DXGI_FORMAT sint, DXGI_FORMAT flt) {
        switch (t) {
        case 1: case 5: return snorm;
        case 3: return sint;
        case 4: return uint;
        case 7: return flt;
        default: return unorm;
        }
    };
    TexFormatInfo i;
    switch (format) {
    case 0x01: i.dxgi = pick(DXGI_FORMAT_R32G32B32A32_FLOAT, DXGI_FORMAT_R32G32B32A32_FLOAT, DXGI_FORMAT_R32G32B32A32_UINT,
                            DXGI_FORMAT_R32G32B32A32_SINT, DXGI_FORMAT_R32G32B32A32_FLOAT);
        i.bytes_per_block = 16; break;
    case 0x02: i.dxgi = pick(DXGI_FORMAT_R32G32B32_FLOAT, DXGI_FORMAT_R32G32B32_FLOAT, DXGI_FORMAT_R32G32B32_UINT,
                            DXGI_FORMAT_R32G32B32_SINT, DXGI_FORMAT_R32G32B32_FLOAT);
        i.bytes_per_block = 12; break;
    case 0x03: i.dxgi = pick(DXGI_FORMAT_R16G16B16A16_UNORM, DXGI_FORMAT_R16G16B16A16_SNORM, DXGI_FORMAT_R16G16B16A16_UINT,
                            DXGI_FORMAT_R16G16B16A16_SINT, DXGI_FORMAT_R16G16B16A16_FLOAT);
        i.bytes_per_block = 8; break;
    case 0x04: i.dxgi = pick(DXGI_FORMAT_R32G32_FLOAT, DXGI_FORMAT_R32G32_FLOAT, DXGI_FORMAT_R32G32_UINT,
                            DXGI_FORMAT_R32G32_SINT, DXGI_FORMAT_R32G32_FLOAT);
        i.bytes_per_block = 8; break;
    case 0x07:
    case 0x08: i.dxgi = pick(srgb ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_SNORM,
                            DXGI_FORMAT_R8G8B8A8_UINT, DXGI_FORMAT_R8G8B8A8_SINT, DXGI_FORMAT_R8G8B8A8_UNORM);
        i.bytes_per_block = 4; break;
    case 0x09: i.dxgi = t == 4 ? DXGI_FORMAT_R10G10B10A2_UINT : DXGI_FORMAT_R10G10B10A2_UNORM; i.bytes_per_block = 4; break;
    case 0x0C: i.dxgi = pick(DXGI_FORMAT_R16G16_UNORM, DXGI_FORMAT_R16G16_SNORM, DXGI_FORMAT_R16G16_UINT,
                            DXGI_FORMAT_R16G16_SINT, DXGI_FORMAT_R16G16_FLOAT);
        i.bytes_per_block = 4; break;
    case 0x0F: i.dxgi = pick(DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32_UINT, DXGI_FORMAT_R32_SINT,
                            DXGI_FORMAT_R32_FLOAT);
        i.bytes_per_block = 4; break;
    case 0x10: i.dxgi = DXGI_FORMAT_BC6H_SF16; i.block_w = i.block_h = 4; i.bytes_per_block = 16; break;
    case 0x11: i.dxgi = DXGI_FORMAT_BC6H_UF16; i.block_w = i.block_h = 4; i.bytes_per_block = 16; break;
    case 0x12: i.dxgi = DXGI_FORMAT_B4G4R4A4_UNORM; i.bytes_per_block = 2; break;
    case 0x13: i.dxgi = DXGI_FORMAT_B5G5R5A1_UNORM; i.bytes_per_block = 2; break;
    case 0x14: i.dxgi = DXGI_FORMAT_B5G5R5A1_UNORM; i.bytes_per_block = 2; break;
    case 0x15: i.dxgi = DXGI_FORMAT_B5G6R5_UNORM; i.bytes_per_block = 2; break;
    case 0x17: i.dxgi = srgb ? DXGI_FORMAT_BC7_UNORM_SRGB : DXGI_FORMAT_BC7_UNORM; i.block_w = i.block_h = 4; i.bytes_per_block = 16; break;
    case 0x18: i.dxgi = pick(DXGI_FORMAT_R8G8_UNORM, DXGI_FORMAT_R8G8_SNORM, DXGI_FORMAT_R8G8_UINT, DXGI_FORMAT_R8G8_SINT,
                            DXGI_FORMAT_R8G8_UNORM);
        i.bytes_per_block = 2; break;
    case 0x1B: i.dxgi = pick(DXGI_FORMAT_R16_UNORM, DXGI_FORMAT_R16_SNORM, DXGI_FORMAT_R16_UINT, DXGI_FORMAT_R16_SINT,
                            DXGI_FORMAT_R16_FLOAT);
        i.bytes_per_block = 2; break;
    case 0x1C:
    case 0x1D: i.dxgi = pick(DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_R8_SNORM, DXGI_FORMAT_R8_UINT, DXGI_FORMAT_R8_SINT,
                            DXGI_FORMAT_R8_UNORM);
        i.bytes_per_block = 1; break;
    case 0x20: i.dxgi = DXGI_FORMAT_R9G9B9E5_SHAREDEXP; i.bytes_per_block = 4; break;
    case 0x21: i.dxgi = DXGI_FORMAT_R11G11B10_FLOAT; i.bytes_per_block = 4; break;
    case 0x24: i.dxgi = srgb ? DXGI_FORMAT_BC1_UNORM_SRGB : DXGI_FORMAT_BC1_UNORM; i.block_w = i.block_h = 4; i.bytes_per_block = 8; break;
    case 0x25: i.dxgi = srgb ? DXGI_FORMAT_BC2_UNORM_SRGB : DXGI_FORMAT_BC2_UNORM; i.block_w = i.block_h = 4; i.bytes_per_block = 16; break;
    case 0x26: i.dxgi = srgb ? DXGI_FORMAT_BC3_UNORM_SRGB : DXGI_FORMAT_BC3_UNORM; i.block_w = i.block_h = 4; i.bytes_per_block = 16; break;
    case 0x27: i.dxgi = t == 1 ? DXGI_FORMAT_BC4_SNORM : DXGI_FORMAT_BC4_UNORM; i.block_w = i.block_h = 4; i.bytes_per_block = 8; break;
    case 0x28: i.dxgi = t == 1 ? DXGI_FORMAT_BC5_SNORM : DXGI_FORMAT_BC5_UNORM; i.block_w = i.block_h = 4; i.bytes_per_block = 16; break;
    case 0x29: case 0x2A: case 0x2B:
        i.dxgi = DXGI_FORMAT_R24_UNORM_X8_TYPELESS; i.bytes_per_block = 4; i.depth = true; break;
    case 0x2F: i.dxgi = DXGI_FORMAT_R32_FLOAT; i.bytes_per_block = 4; i.depth = true; break;
    case 0x30: i.dxgi = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS; i.bytes_per_block = 8; i.depth = true; break;
    case 0x3A: i.dxgi = DXGI_FORMAT_R16_UNORM; i.bytes_per_block = 2; i.depth = true; break;
    default: break;
    }
    // ASTC: decoded to RGBA8 on upload
    static const struct { u32 f, w, h; } astc[] = {
        {0x40, 4, 4}, {0x41, 5, 5}, {0x42, 6, 6}, {0x44, 8, 8}, {0x45, 10, 10}, {0x46, 12, 12}, {0x50, 5, 4},
        {0x51, 6, 5}, {0x52, 8, 6}, {0x53, 10, 8}, {0x54, 12, 10}, {0x55, 8, 5}, {0x56, 10, 5}, {0x57, 10, 6}};
    for (auto& a : astc)
        if (a.f == format) {
            i.dxgi = srgb ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
            i.block_w = a.w;
            i.block_h = a.h;
            i.bytes_per_block = 16;
            i.astc = true;
        }
    return i;
}

static inline u32 GobOffset(u32 x, u32 y) {
    return ((x % 64) / 32) * 256 + ((y % 8) / 2) * 64 + ((x % 32) / 16) * 32 + (y % 2) * 16 + (x % 16);
}

u64 BlockLinearSize(u32 width, u32 height, u32 depth, u32 bpb, u32 bh, u32 bd) {
    const u32 gobs_x = (width * bpb + 63) / 64;
    const u32 gobs_y = (height + 7) / 8;
    const u32 blocks_y = (gobs_y + (1u << bh) - 1) >> bh;
    const u32 blocks_z = (depth + (1u << bd) - 1) >> bd;
    return u64(gobs_x) * blocks_y * blocks_z * (512ull << (bh + bd));
}

void Deswizzle(const u8* src, u8* dst, u32 width, u32 height, u32 depth, u32 bpb, u32 bh, u32 bd, u32 dst_pitch) {
    const u32 width_bytes = width * bpb;
    const u32 gobs_x = (width_bytes + 63) / 64;
    const u32 gobs_y = (height + 7) / 8;
    const u32 blocks_y = (gobs_y + (1u << bh) - 1) >> bh;
    const u64 block_size = 512ull << (bh + bd);
    const u32 block_h_rows = 8u << bh;
    for (u32 z = 0; z < depth; z++) {
        const u32 block_z = z >> bd, gob_z = z & ((1u << bd) - 1);
        for (u32 y = 0; y < height; y++) {
            const u32 block_y = y / block_h_rows;
            const u32 gob_y = (y % block_h_rows) / 8;
            const u64 row_base = (u64(block_z) * blocks_y + block_y) * gobs_x * block_size +
                                 (u64(gob_z) * (1u << bh) + gob_y) * 512;
            u8* drow = dst + (u64(z) * height + y) * dst_pitch;
            for (u32 x = 0; x < width_bytes; x += 16) {
                // 16-byte runs are contiguous inside a GOB
                const u32 gob_x = x / 64;
                const u64 off = row_base + u64(gob_x) * block_size + GobOffset(x, y);
                const u32 n = std::min<u32>(16, width_bytes - x);
                std::memcpy(drow + x, src + off, n);
            }
        }
    }
}

} // namespace gpu

namespace gpu {
// Placeholder until the ASTC decoder lands: mid-grey so missing data is visible but not garish.
void DecodeAstc(const u8*, u8* dst, u32 width, u32 height, u32, u32) {
    std::memset(dst, 0x80, size_t(width) * height * 4);
}
} // namespace gpu
