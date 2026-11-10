#include "gpu/nvn_encoder.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "common/common.h"
#include "core/hle/nvn/nvn_hle.h"
#include "core/memory.h"
#include "gpu/gpu.h"
#include "gpu/draw_state.h"
#include "gpu/renderer.h"
#include "gpu/shader/translator.h"

namespace gpu {
std::atomic<u64> g_indirect_known{0}, g_indirect_unknown{0}, g_indirect_arrays{0};


using hle::nvn::ReplayCommand;
using hle::nvn::StateCall;

namespace {

// NVN shader stages (0 vertex, 1 fragment, 2 geometry, 3 tess control, 4 tess evaluation, 5 compute) in hardware
// terms: the constant buffer bind stage and the shader config program slot.
constexpr u32 kHwStage[5] = {0, 4, 3, 1, 2};
constexpr u32 kHwProgram[5] = {1, 5, 4, 2, 3};

// Driver-owned constant buffers (c0 driver constants, c1, c2 texture handles) per hardware stage, in host GPU memory.
constexpr u64 kDrvCbSize = 0x10000;
u64 DrvCb(u32 hw_stage, u32 cb) {
    return Gpu::kHostBase + (u64(hw_stage) * 3 + cb) * kDrvCbSize;
}

u32 F2U(float f) {
    return std::bit_cast<u32>(f);
}
float U2F(u32 u) {
    return std::bit_cast<float>(u);
}

// NVN format -> hardware render target format.
const std::unordered_map<u32, u32> kNvnRtFormat = {
    {0x01, 0xF3}, {0x02, 0xF4}, {0x03, 0xF6}, {0x04, 0xF5}, {0x05, 0xF2}, {0x06, 0xEE}, {0x07, 0xEF}, {0x08, 0xF1},
    {0x09, 0xF0}, {0x0A, 0xE5}, {0x0B, 0xE4}, {0x0C, 0xE3}, {0x0D, 0xEA}, {0x0E, 0xEB}, {0x0F, 0xED}, {0x10, 0xEC},
    {0x11, 0xDE}, {0x12, 0xDA}, {0x13, 0xDB}, {0x14, 0xDD}, {0x15, 0xDC}, {0x16, 0xCB}, {0x17, 0xCD}, {0x18, 0xCC},
    {0x25, 0xD5}, {0x26, 0xD7}, {0x27, 0xD9}, {0x28, 0xD8}, {0x29, 0xCA}, {0x2A, 0xC6}, {0x2B, 0xC7}, {0x2C, 0xC9},
    {0x2D, 0xC8}, {0x2E, 0xC0}, {0x2F, 0xC2}, {0x30, 0xC1}, {0x37, 0xD6}, {0x38, 0xD6}, {0x3C, 0xE8}, {0x3D, 0xD1},
    {0x3E, 0xD2}, {0x3F, 0xE0}};
// NVN depth format -> hardware zeta format.
const std::unordered_map<u32, u32> kNvnZetaFormat = {{0x32, 0x13}, {0x33, 0x14}, {0x34, 0x0A}, {0x35, 0x14}, {0x36, 0x19}};

// NVN vertex format -> hardware attribute size (bits 21-26) and type (bits 27-29), or 0.
u32 AttribFormat(u32 f) {
    enum : u32 { SN = 1, UN = 2, SI = 3, UI = 4, F = 7 };
    struct E {
        u32 size, type;
    };
    static const std::unordered_map<u32, E> t = {
        {0x01, {0x1D, UN}}, {0x02, {0x1D, SN}}, {0x03, {0x1D, UI}}, {0x04, {0x1D, SI}}, {0x05, {0x1B, F}},
        {0x06, {0x1B, UN}}, {0x07, {0x1B, SN}}, {0x08, {0x1B, UI}}, {0x09, {0x1B, SI}}, {0x0A, {0x12, F}},
        {0x0B, {0x12, UI}}, {0x0C, {0x12, SI}}, {0x0D, {0x18, UN}}, {0x0E, {0x18, SN}}, {0x0F, {0x18, UI}},
        {0x10, {0x18, SI}}, {0x11, {0x0F, F}},  {0x12, {0x0F, UN}}, {0x13, {0x0F, SN}}, {0x14, {0x0F, UI}},
        {0x15, {0x0F, SI}}, {0x16, {0x04, F}},  {0x17, {0x04, UI}}, {0x18, {0x04, SI}}, {0x19, {0x13, UN}},
        {0x1A, {0x13, SN}}, {0x1B, {0x13, UI}}, {0x1C, {0x13, SI}}, {0x1D, {0x05, F}},  {0x1E, {0x05, UN}},
        {0x1F, {0x05, SN}}, {0x20, {0x05, UI}}, {0x21, {0x05, SI}}, {0x22, {0x02, F}},  {0x23, {0x02, UI}},
        {0x24, {0x02, SI}}, {0x25, {0x0A, UN}}, {0x26, {0x0A, SN}}, {0x27, {0x0A, UI}}, {0x28, {0x0A, SI}},
        {0x29, {0x03, F}},  {0x2A, {0x03, UN}}, {0x2B, {0x03, SN}}, {0x2C, {0x03, UI}}, {0x2D, {0x03, SI}},
        {0x2E, {0x01, F}},  {0x2F, {0x01, UI}}, {0x30, {0x01, SI}}, {0x3D, {0x30, UN}}, {0x3E, {0x30, UI}},
        {0x3F, {0x31, F}}};
    auto it = t.find(f);
    return it == t.end() ? 0 : (it->second.size << 21) | (it->second.type << 27);
}

// Block-linear tile mode for a surface (block height in GOBs of 8 rows, log2, at most 16 GOBs).
u32 TileMode(u32 height) {
    u32 bh = 0;
    while (bh < 4 && (8u << bh) < height)
        bh++;
    return bh << 4;
}

// NVN format -> bytes per block and block size in texels (compressed formats use 4x4 blocks).
struct BlockInfo {
    u32 bpb = 0, bw = 1, bh = 1;
};
BlockInfo NvnBlock(u32 f) {
    if (f >= 0x01 && f <= 0x04) return {1};
    if (f >= 0x05 && f <= 0x09) return {2};
    if (f >= 0x0A && f <= 0x0C) return {4};
    if (f >= 0x0D && f <= 0x10) return {2};
    if (f >= 0x11 && f <= 0x15) return {4};
    if (f >= 0x16 && f <= 0x18) return {8};
    if (f >= 0x19 && f <= 0x1C) return {3};
    if (f >= 0x1D && f <= 0x21) return {6};
    if (f >= 0x22 && f <= 0x24) return {12};
    if (f >= 0x25 && f <= 0x28) return {4};
    if (f >= 0x29 && f <= 0x2D) return {8};
    if (f >= 0x2E && f <= 0x30) return {16};
    switch (f) {
    case 0x31: return {1};
    case 0x32: return {2};
    case 0x33: case 0x34: case 0x35: return {4};
    case 0x36: return {8};
    case 0x37: case 0x38: return {4};
    case 0x39: case 0x3A: case 0x3B: case 0x3C: return {2};
    case 0x3D: case 0x3E: case 0x3F: case 0x40: return {4};
    case 0x41: case 0x42: case 0x45: case 0x46: case 0x49: case 0x4A: return {8, 4, 4};
    case 0x43: case 0x44: case 0x47: case 0x48: case 0x4B: case 0x4C: return {16, 4, 4};
    case 0x4D: case 0x4E: case 0x4F: case 0x50: return {16, 4, 4};
    default: return {};
    }
}

// Where one level (and array layer) of a block-linear texture lives, in blocks.
struct TexLevel {
    u64 addr = 0;
    u32 bh = 0, bd = 0;            // block height / depth in GOBs, log2
    u32 wb = 0, hb = 0, depth = 1; // level size in blocks (depth in slices for 3D)
    BlockInfo blk;
};
bool LevelLayout(const hle::nvn::TextureInfo& t, u32 level, u32 layer, TexLevel& out) {
    const BlockInfo b = NvnBlock(t.format);
    if (!b.bpb || !t.width)
        return false;
    const bool is3d = t.target == 2;
    const u32 depth = is3d ? std::max<u32>(t.depth, 1) : 1;
    const u32 hb0 = (std::max<u32>(t.height, 1) + b.bh - 1) / b.bh;
    u32 bh0 = 0, bd0 = 0;
    // The driver's choice, as its texture descriptors show: a block may be up to a third shorter than the texture
    // (20 rows of blocks use 16-row blocks, 22 use 32-row ones). Copies must match or the image lands scrambled.
    while (bh0 < 4 && (8u << bh0) * 4 < hb0 * 3)
        bh0++;
    while (is3d && bd0 < 5 && (1u << bd0) < depth)
        bd0++;
    u64 offset = 0, layer_size = 0;
    for (u32 l = 0; l < std::max<u32>(t.levels, 1); l++) {
        const u32 w = std::max<u32>(t.width >> l, 1), h = std::max<u32>(t.height >> l, 1), d = std::max<u32>(depth >> l, 1);
        TexLevel lv;
        lv.blk = b;
        lv.wb = (w + b.bw - 1) / b.bw, lv.hb = (h + b.bh - 1) / b.bh, lv.depth = d;
        lv.bh = bh0, lv.bd = bd0;
        while (lv.bh > 0 && (8u << (lv.bh - 1)) >= lv.hb)
            lv.bh--;
        while (lv.bd > 0 && (1u << (lv.bd - 1)) >= lv.depth)
            lv.bd--;
        const u64 size = u64((lv.wb * b.bpb + 63) / 64) * ((lv.hb + (8u << lv.bh) - 1) / (8u << lv.bh)) *
                         ((lv.depth + (1u << lv.bd) - 1) >> lv.bd) * (512ull << (lv.bh + lv.bd));
        if (l == level) {
            out = lv;
            out.addr = offset;
        }
        offset += size;
    }
    layer_size = common::AlignUp(offset, 512ull << (bh0 + bd0));
    if (level >= std::max<u32>(t.levels, 1))
        return false;
    out.addr += t.gpu + u64(layer) * layer_size;
    return true;
}

// DMA engine launch: the registers RunDma reads, and the LAUNCH value.
struct DmaLaunch {
    std::array<u32, 0x200> r{};
    u32 launch = 0;
};

// One submit's commands: the recorder's shared lists (kept alive) and the commands in execution order.
struct Frame {
    std::vector<hle::nvn::ReplaySpan> spans;
    std::vector<const ReplayCommand*> cmds;

    explicit Frame(std::vector<hle::nvn::ReplaySpan>&& s = {}) : spans(std::move(s)) {
        for (const auto& sp : spans)
            for (size_t i = sp.begin; i < sp.end; i++)
                cmds.push_back(&(*sp.list)[i]);
    }
    size_t size() const { return cmds.size(); }
    const ReplayCommand& operator[](size_t i) const { return *cmds[i]; }
};

bool Is(std::string_view name, std::string_view suffix) {
    return name.size() >= suffix.size() && name.substr(name.size() - suffix.size()) == suffix;
}

// GPU addresses have 40 bits.
constexpr u64 Va(u64 a) {
    return a & 0xFFFFFFFFFFull;
}

// ---------------------------------------------------------------- encoder
class Encoder {
public:
    explicit Encoder(Gpu& g) : g_(g), s_(g.State()) {
        // driver constant buffers c0-c2 of every graphics stage
        // (as the driver: one c0 shared by all stages, c1 = the bound program's constants, none for tessellation)
        for (u32 hw = 0; hw < 5; hw++) {
            Bind(hw, 0, DrvCb(0, 0), 0x7000);
            Bind(hw, 2, DrvCb(hw, 2), 0x200);
            for (u32 i = 1; i < 8; i++)
                DrvWrite(hw, 2, i * 4, i);
        }
        for (u32 i = 1; i < 8; i++)
            DrvWrite(5, 2, i * 4, i);
        DrvWrite(0, 0, 0x610, 0x44FFFE00u); // driver constant (seen in every draw)
        ResetAttribs();
    }

    void Run(const Frame& cmds) {
        for (size_t i = 0; i < cmds.size(); i++)
            Encode(cmds[i]);
    }

private:
    void Bind(u32 hw, u32 index, u64 addr, u32 size) {
        if (index >= 18)
            return;
        auto& b = s_.cb[hw][index];
        b.enabled = addr && size;
        b.address = Va(addr);
        b.size = size;
    }
    template <class T>
    void DrvWrite(u32 hw, u32 cb, u32 offset, const T& v) {
        if (offset + sizeof(T) <= kDrvCbSize)
            std::memcpy(g_.HostMem() + (DrvCb(hw, cb) - Gpu::kHostBase) + offset, &v, sizeof(T));
    }
    void Unhandled(std::string name) {
        if (unhandled_.insert(name).second)
            LOG_WARN("NVN", "encoder ignores: {}", name);
    }

    void Encode(const ReplayCommand& c) {
        // (a prefix test: searching each command's name for "CommandBuffer" was ~2.5% of the GPU thread)
        const std::string_view n = c.name.substr(c.name.substr(0, 16) == "nvnCommandBuffer" ? 16 : 0);
        const u64* x = c.x;
        if (n == "BindProgram")
            BindProgram(c);
        else if (n == "BindUniformBuffer") {
            if (x[1] < 5)
                Bind(kHwStage[x[1]], u32(x[2]) + 3, x[3], u32(x[4]));
            else if (x[1] == 5 && x[2] < 5) {
                cs_ubo_[x[2]] = {x[3], u32(x[4])};
                // compute's c0 also lists them: {address lo, hi, size, 0} at 0x210
                const u32 off = 0x210 + u32(x[2]) * 16;
                DrvWrite(5, 0, off, u32(x[3]));
                DrvWrite(5, 0, off + 4, u32(x[3] >> 32));
                DrvWrite(5, 0, off + 8, u32(x[4]));
            }
        } else if (n == "BindStorageBuffer" && x[1] == 5) {
            if (x[2] < 16) { // compute's own c0: the storage buffer table at 0x310
                const u32 off = 0x310 + u32(x[2]) * 16;
                DrvWrite(5, 0, off, u32(x[3]));
                DrvWrite(5, 0, off + 4, u32(x[3] >> 32));
                DrvWrite(5, 0, off + 8, u32(x[4]));
            }
        } else if (n == "BindStorageBuffer") {
            // the driver's table in the shared c0: {address lo, hi, size, 0} at 0x110 + hardware stage * 0x100
            if (x[1] < 5 && x[2] < 16) {
                const u32 off = 0x110 + kHwStage[x[1]] * 0x100 + u32(x[2]) * 16;
                DrvWrite(0, 0, off, u32(x[3]));
                DrvWrite(0, 0, off + 4, u32(x[3] >> 32));
                DrvWrite(0, 0, off + 8, u32(x[4]));
            }
        } else if (n == "BindTexture" || n == "BindImage") {
            if (x[1] < 5 && x[2] < 128)
                DrvWrite(kHwStage[x[1]], 2, (n == "BindImage" ? 0x120 : 0x20) + u32(x[2]) * 8, u32(x[3]));
            else if (x[1] == 5 && x[2] < 128) // compute keeps the whole 64-bit handle
                DrvWrite(5, 2, (n == "BindImage" ? 0x120 : 0x20) + u32(x[2]) * 8, x[3]);
        } else if (n == "BindVertexBuffer") {
            if (x[1] < 16) {
                auto& st = s_.streams[x[1]];
                if (st.enabled != (x[2] != 0))
                    s_.vertex_format_serial++;
                st.enabled = x[2] != 0;
                st.addr = Va(x[2]);
                st.limit = Va(x[2] + (x[3] ? x[3] - 1 : 0));
            }
        } else if (n == "BindVertexAttribState")
            BindAttribs(c);
        else if (n == "BindVertexStreamState")
            BindStreams(c);
        else if (n == "SetRenderTargets")
            SetRenderTargets(c);
        else if (n == "SetViewports" || n == "SetDepthRanges") {
            const bool vp = n == "SetViewports";
            for (u32 i = 0; i < u32(x[2]) && x[1] + i < 16; i++) {
                const u32 k = u32(x[1]) + i;
                if (vp) {
                    for (u32 j = 0; j < 4; j++)
                        viewport_[k][j] = U2F(Raw32(c, i * 4 + j));
                } else {
                    depth_range_[k][0] = U2F(Raw32(c, i * 2));
                    depth_range_[k][1] = U2F(Raw32(c, i * 2 + 1));
                }
                if (k == 0)
                    WriteViewport();
            }
        } else if (n == "SetScissors") {
            for (u32 i = 0; i < u32(x[2]) && x[1] + i < 16; i++)
                if (x[1] + i == 0)
                    SetScissor(Raw32(c, i * 4), Raw32(c, i * 4 + 1), Raw32(c, i * 4 + 2), Raw32(c, i * 4 + 3));
        } else if (n == "SetScissor")
            SetScissor(u32(x[1]), u32(x[2]), u32(x[3]), u32(x[4]));
        else if (n == "BindBlendState")
            ApplyBlend(c);
        else if (n == "BindColorState")
            ApplyColor(c);
        else if (n == "BindChannelMaskState")
            ApplyChannelMask(c);
        else if (n == "BindDepthStencilState")
            ApplyDepthStencil(c);
        else if (n == "BindPolygonState")
            ApplyPolygon(c);
        else if (n == "BindMultisampleState" || n == "Barrier" || n == "SetDepthBounds" || n == "SetPatchSize" ||
                 n == "SetShaderScratchMemory" || n == "SetRenderEnable" || n == "SetRenderEnableConditional" ||
                 n == "SetPrimitiveRestart") {
            // not applied by the renderer (render enables: every draw is drawn)
        } else if (n == "ReportCounter") { // (cb, counter type, address) -> {value, GPU timestamp}
            const u64 v[2] = {0, g_.GpuTimestamp()};
            g_.WriteGpu(x[2], v, sizeof(v));
        } else if (n == "SetStencilRef" || n == "SetStencilValueMask" || n == "SetStencilMask") {
            // Direct3D has one reference and one pair of masks: the front face's
            if (x[1] & 1) {
                u32& v = n == "SetStencilRef" ? s_.stencil_ref : n == "SetStencilMask" ? s_.stencil_write_mask : s_.stencil_read_mask;
                if (v != u32(x[2]) && n != "SetStencilRef")
                    s_.depth_serial++;
                v = u32(x[2]);
            }
        } else if (n == "SetPolygonOffsetClamp") {
            s_.slope_scale_depth_bias = U2F(c.v[0]);
            s_.depth_bias = U2F(c.v[1]);
            s_.depth_bias_clamp = U2F(c.v[2]);
            s_.raster_serial++;
        } else if (n == "SetTexturePool" || n == "SetSamplerPool") {
            (n == "SetTexturePool" ? s_.tic_base : s_.tsc_base) = Va(hle::nvn::QueryTexturePool(x[1]));
        } else if (n == "ClearColor") {
            const float color[4] = {U2F(Raw32(c, 0)), U2F(Raw32(c, 1)), U2F(Raw32(c, 2)), U2F(Raw32(c, 3))};
            if ((x[3] & 0xF) && g_.GetRenderer())
                g_.GetRenderer()->ClearColor(u32(x[1]) & 0xF, color);
        } else if (n == "ClearDepthStencil") {
            if ((x[1] || x[3]) && g_.GetRenderer())
                g_.GetRenderer()->ClearDepthStencil(x[1] != 0, x[3] != 0, U2F(c.v[0]), u8(x[2]));
        } else if (n == "SetCopyRowStride")
            row_stride_ = u32(x[1]);
        else if (n == "SetCopyImageStride")
            image_stride_ = u32(x[1]);
        else if (n == "CopyBufferToBuffer") { // (cb, src, dst, size, flags)
            DmaLaunch l;
            l.r[0x100] = u32(x[1] >> 32), l.r[0x101] = u32(x[1]);
            l.r[0x102] = u32(x[2] >> 32), l.r[0x103] = u32(x[2]);
            l.r[0x106] = u32(x[3]);
            l.launch = (1u << 7) | (1u << 8);
            g_.RunDma(l.r.data(), l.launch);
        } else if (n == "CopyBufferToTexture" || n == "CopyTextureToBuffer")
            CopyTexture(c, n == "CopyBufferToTexture");
        else if (n == "DispatchCompute")
            Dispatch(u32(x[1]), u32(x[2]), u32(x[3]));
        else if (n == "DrawElementsBaseVertex")
            Draw(u32(x[1]), true, 0, u32(x[3]), u32(x[2]), x[4], s32(x[5]), 1, 0);
        else if (n == "DrawElements")
            Draw(u32(x[1]), true, 0, u32(x[3]), u32(x[2]), x[4], 0, 1, 0);
        else if (n == "DrawArrays")
            Draw(u32(x[1]), false, u32(x[2]), u32(x[3]), 0, 0, 0, 1, 0);
        else if (n == "DrawElementsIndirect") {
            u32 a[5]{}; // count, instances, first index, base vertex, base instance
            // The culling list draws: the host wrote the list and knows the count, so the arguments the GPU
            // produced from its counter are not read back (that read waits for the whole frame so far).
            u32 known = 0;
            if (g_.GetRenderer())
                known = g_.GetRenderer()->CullListCount(x[3]);
            if (known) {
                g_indirect_known++;
                Draw(u32(x[1]), true, 0, known, u32(x[2]), x[3], 0, 1, 0);
            } else {
                g_indirect_unknown++;
                SyncForRead(x[4], sizeof(a));
                g_.ReadGpu(x[4], a, sizeof(a));
                static int logged = 0;
                if (logged++ < 30)
                    LOG_INFO("NVN", "indirect elements not from the host's culling list: args {:#x} count {} instances {} first {} base {} indices {:#x} type {} mode {}",
                             x[4], a[0], a[1], a[2], a[3], x[3], x[2], x[1]);
                Draw(u32(x[1]), true, a[2], a[0], u32(x[2]), x[3], s32(a[3]), a[1], a[4]);
            }
        } else if (n == "DrawArraysIndirect") {
            g_indirect_arrays++;
            // arguments written by compute: the renderer draws indirectly from the resident copy (or reads them)
            s_.draw.indirect_args = x[2];
            Draw(u32(x[1]), false, 0, 1, 0, 0, 0, 1, 0);
            s_.draw.indirect_args = 0;
        } else
            Unhandled(std::string(n));
    }

    // Indirect arguments are often written by compute just before: land those results first.
    void SyncForRead(u64 addr, u64 size) {
        if (g_.GetRenderer())
            g_.GetRenderer()->SyncRegion(addr, size);
    }

    u32 Raw32(const ReplayCommand& c, u32 i) const {
        return i / 2 < c.raw.size() ? u32(c.raw[i / 2] >> ((i & 1) * 32)) : 0;
    }

    void BindProgram(const ReplayCommand& c) {
        u64 code = 0, control = 0;
        if (c.x[1]) {
            if (c.extra.size() >= 2) // the program's first stage {code, control}, captured at bind
                code = c.extra[0], control = c.extra[1];
            if (!code)
                code = hle::nvn::QueryProgramCode(c.x[1]);
        }
        for (u32 s = 0; s < kNumGraphicsStages; s++)
            if (c.x[2] & (1u << s))
                s_.program[s] = code ? Va(code + 0x30) : 0; // the Maxwell program header follows NVN's 0x30-byte blob header
        if (c.x[2] & 0x20)
            cs_code_ = code, cs_control_ = control;
        program_constants_dirty_ = true;
    }

    // Texture <-> buffer copies, one DMA launch per slice / layer: linear rows of whole blocks on the buffer side.
    void CopyTexture(const ReplayCommand& c, bool upload) {
        const u64 buffer = upload ? c.x[1] : c.x[4], tex = upload ? c.x[2] : c.x[1];
        hle::nvn::TextureInfo t;
        if (!hle::nvn::QueryTexture(tex, t)) {
            Unhandled("copy: texture not in the model");
            return;
        }
        s32 region[6]{}; // x, y, z, width, height, depth
        for (u32 i = 0; i < 6; i++)
            region[i] = s32(Raw32(c, i));
        u32 level = 0, min_layer = 0;
        // the view's raw words follow the region: word 0 = flags | base level << 8 | level count << 16
        if (Raw32(c, 6) & 1)
            level = (Raw32(c, 6) >> 8) & 0xFF;
        if (!c.objects.empty())
            for (const StateCall& k : c.objects[0]) {
                if (Is(k.name, "SetLevels"))
                    level = u32(k.x[1]);
                else if (Is(k.name, "SetLayers"))
                    min_layer = u32(k.x[1]);
            }
        const bool is3d = t.target == 2;
        for (s32 z = region[2]; z < region[2] + std::max(region[5], 1); z++) {
            TexLevel lv;
            if (!LevelLayout(t, level, is3d ? 0 : min_layer + u32(z), lv)) {
                Unhandled(fmt::format("copy: layout of format {:#x} target {}", t.format, t.target));
                return;
            }
            const u32 wb = (u32(region[3]) + lv.blk.bw - 1) / lv.blk.bw, hb = (u32(region[4]) + lv.blk.bh - 1) / lv.blk.bh;
            const u32 row = row_stride_ ? row_stride_ : wb * lv.blk.bpb;
            const u32 image = image_stride_ ? image_stride_ : row * hb;
            const u64 linear = buffer + u64(z - region[2]) * image;
            DmaLaunch l;
            const u32 b = upload ? 0x1C3 : 0x1CA;
            const u64 src = upload ? linear : lv.addr, dst = upload ? lv.addr : linear;
            l.r[0x100] = u32(src >> 32), l.r[0x101] = u32(src);
            l.r[0x102] = u32(dst >> 32), l.r[0x103] = u32(dst);
            l.r[upload ? 0x104 : 0x105] = row;
            l.r[0x106] = wb * lv.blk.bpb;
            l.r[0x107] = hb;
            l.r[b] = (lv.bh << 4) | (lv.bd << 8);
            l.r[b + 1] = lv.wb * lv.blk.bpb;
            l.r[b + 2] = lv.hb;
            l.r[b + 3] = lv.depth;
            l.r[b + 4] = is3d ? u32(z) : 0;
            l.r[b + 5] = ((u32(region[0]) / lv.blk.bw) * lv.blk.bpb) | ((u32(region[1]) / lv.blk.bh) << 16);
            l.launch = (1u << 9) | (upload ? 1u << 7 : 1u << 8);
            g_.RunDma(l.r.data(), l.launch);
        }
    }

    void Dispatch(u32 gx, u32 gy, u32 gz) {
        // Compute code has no blob header; the launch shape and constants come from NVN's control section.
        const ComputeInfo& ci = Compute(cs_code_, cs_control_);
        ComputeLaunch l;
        l.program = Va(cs_code_);
        l.program_offset = u32(cs_code_ & 0xFFFF);
        l.grid[0] = gx & 0x7FFFFFFF, l.grid[1] = gy & 0xFFFF, l.grid[2] = gz & 0xFFFF;
        l.block[0] = ci.block[0], l.block[1] = ci.block[1], l.block[2] = ci.block[2];
        l.shared_bytes = ci.shared & 0x3FFFF;
        l.tic_base = s_.tic_base, l.tsc_base = s_.tsc_base;
        DrvWrite(5, 0, 0x200, gx);
        DrvWrite(5, 0, 0x204, gy);
        DrvWrite(5, 0, 0x208, gz);
        auto slot = [&](u32 i, u64 addr, u32 size) {
            if (!addr || !size)
                return;
            l.cb_addr[i] = Va(addr);
            l.cb_size[i] = std::min<u32>(size, 0x10000);
        };
        slot(0, DrvCb(5, 0), 0x1000);
        slot(1, ci.constants_size ? cs_code_ + ci.constants : 0, ci.constants_size);
        slot(kTextureHandleCb, DrvCb(5, 2), 0x600);
        for (u32 i = 0; i < 5; i++)
            slot(3 + i, cs_ubo_[i].addr, cs_ubo_[i].size);
        if (g_.GetRenderer())
            g_.GetRenderer()->Dispatch(l);
    }

    void ResetAttribs() {
        for (u32 i = 0; i < 32; i++)
            s_.attrib[i] = i < 16 ? 0x3A400040u : 0; // the driver's "constant input" value
    }

    // NVNvertexAttribState (4 bytes): stream index bits 0-4, "set" bit 6, offset bits 7-20, NVN format from bit 21.
    void BindAttribs(const ReplayCommand& c) {
        const u32 count = std::min<u32>(u32(c.x[1]), 32);
        u32 attrib[32];
        for (u32 i = 0; i < 32; i++)
            attrib[i] = i < 16 ? 0x3A400040u : 0;
        for (u32 i = 0; i < count; i++) {
            const u32 w = Raw32(c, i);
            const u32 fmt = w >> 21, offset = (w >> 7) & 0x3FFF, stream = w & 0x1F;
            const u32 hw = AttribFormat(fmt);
            if (!hw) {
                if (fmt)
                    Unhandled(fmt::format("vertex format {:#x}", fmt));
                continue;
            }
            attrib[i] = stream | (offset << 7) | hw;
        }
        if (std::memcmp(attrib, s_.attrib, sizeof(attrib)) != 0) {
            std::memcpy(s_.attrib, attrib, sizeof(attrib));
            s_.vertex_format_serial++;
        }
    }

    // NVNvertexStreamState (8 bytes): stride, divisor.
    void BindStreams(const ReplayCommand& c) {
        const u32 count = std::min<u32>(u32(c.x[1]), 16);
        for (u32 i = 0; i < count; i++) {
            const u32 divisor = Raw32(c, i * 2 + 1);
            if (s_.streams[i].divisor != divisor)
                s_.vertex_format_serial++;
            s_.streams[i].stride = Raw32(c, i * 2) & 0xFFF, s_.streams[i].divisor = divisor;
        }
    }

    void SetRenderTargets(const ReplayCommand& c) {
        const u32 count = std::min<u32>(u32(c.x[1]), 8);
        for (u32 i = 0; i < 8; i++) {
            hle::nvn::TextureInfo t;
            auto& rt = s_.color[i];
            if (i >= count || i >= c.extra.size() || !c.extra[i] || !hle::nvn::QueryTexture(c.extra[i], t)) {
                rt.format = 0;
                continue;
            }
            auto f = kNvnRtFormat.find(t.format);
            if (f == kNvnRtFormat.end())
                Unhandled(fmt::format("rt format {:#x}", t.format));
            rt.addr = Va(t.gpu);
            rt.width = t.width;
            rt.height = t.height;
            rt.format = f == kNvnRtFormat.end() ? 0 : f->second;
            rt.tile = TileMode(t.height);
        }
        s_.rt_count = count;
        hle::nvn::TextureInfo d;
        if (c.x[4] && hle::nvn::QueryTexture(c.x[4], d)) {
            auto f = kNvnZetaFormat.find(d.format);
            if (f == kNvnZetaFormat.end())
                Unhandled(fmt::format("depth format {:#x}", d.format));
            s_.depth.addr = Va(d.gpu);
            s_.depth.format = f == kNvnZetaFormat.end() ? 0 : f->second;
            s_.depth.width = d.width;
            s_.depth.height = d.height;
            s_.depth.enabled = true;
        } else
            s_.depth.enabled = false;
    }

    void WriteViewport() {
        const float x = viewport_[0][0], y = viewport_[0][1], w = viewport_[0][2], h = viewport_[0][3];
        const float n = depth_range_[0][0], f = depth_range_[0][1];
        s_.vp_scale[0] = w * 0.5f, s_.vp_scale[1] = -h * 0.5f, s_.vp_scale[2] = (f - n) * 0.5f;
        s_.vp_translate[0] = x + w * 0.5f, s_.vp_translate[1] = y + h * 0.5f, s_.vp_translate[2] = (f + n) * 0.5f;
        s_.depth_near = n, s_.depth_far = f;
    }

    void SetScissor(u32 x, u32 y, u32 w, u32 h) {
        s_.scissor_enable = true;
        s_.scissor[0] = x & 0xFFFF, s_.scissor[1] = y & 0xFFFF;
        s_.scissor[2] = (x + w) & 0xFFFF, s_.scissor[3] = (y + h) & 0xFFFF;
    }

    // State objects are decoded from their raw words (captured at bind; the game builds many on the stack).
    // NVNblendState: byte 0 target, byte 3 equations (rgb | alpha << 4), bytes 4-7 src/dst rgb, src/dst alpha.
    void ApplyBlend(const ReplayCommand& c) {
        if (c.raw.empty())
            return;
        const u64 w = c.raw[0];
        if (w == last_blend_[u32(w) & 7])
            return;
        last_blend_[u32(w) & 7] = w;
        s_.blend_serial++;
        auto& b = s_.blend[u32(w) & 7];
        b.eq_rgb = u32(w >> 24) & 0xF, b.eq_a = u32(w >> 28) & 0xF;
        b.src_rgb = u32(w >> 32) & 0xFF, b.dst_rgb = u32(w >> 40) & 0xFF;
        b.src_a = u32(w >> 48) & 0xFF, b.dst_a = u32(w >> 56) & 0xFF;
    }

    // NVNcolorState: byte 1 blend enable per target, byte 3 alpha test function (8 = always).
    void ApplyColor(const ReplayCommand& c) {
        if (c.raw.empty())
            return;
        const u32 w = u32(c.raw[0]);
        if (w == last_color_)
            return;
        last_color_ = w;
        s_.blend_serial++;
        const u32 enable = (w >> 8) & 0xFF, alpha_func = (w >> 24) & 0xF;
        for (u32 i = 0; i < 8; i++)
            s_.blend_enable[i] = (enable >> i) & 1;
        s_.alpha_test = alpha_func && alpha_func != 8;
        s_.alpha_func = s_.alpha_test ? alpha_func : 0;
    }

    // NVNchannelMaskState: 4 bits (r, g, b, a) per target.
    void ApplyChannelMask(const ReplayCommand& c) {
        if (c.raw.empty())
            return;
        const u32 w = u32(c.raw[0]);
        if (w == last_mask_)
            return;
        last_mask_ = w;
        s_.blend_serial++;
        for (u32 i = 0; i < 8; i++)
            s_.color_mask[i] = (w >> (i * 4)) & 0xF;
    }

    // NVNdepthStencilState: bit 0 depth test, 1 depth write, 2 stencil test, bits 4-7 depth function; from bit 32 one
    // 16-bit word per face (front, back): function, fail, pass, depth fail ops (4 bits each).
    void ApplyDepthStencil(const ReplayCommand& c) {
        if (c.raw.empty())
            return;
        const u64 w = c.raw[0];
        if (w == last_depth_)
            return;
        last_depth_ = w;
        s_.depth_serial++;
        s_.depth_test = u32(w) & 1;
        s_.depth_write = (u32(w) >> 1) & 1;
        s_.depth_func = (u32(w) >> 4) & 0xF;
        s_.stencil_test = (u32(w) >> 2) & 1;
        const u32 front = u32(w >> 32) & 0xFFFF, back = u32(w >> 48) & 0xFFFF;
        s_.stencil_front = {front & 0xF, (front >> 4) & 0xF, front >> 12, (front >> 8) & 0xF};
        s_.stencil_back = {back & 0xF, (back >> 4) & 0xF, back >> 12, (back >> 8) & 0xF};
    }

    // NVNpolygonState: bits 0-1 cull face, bit 2 front face counter-clockwise, bits 3-4 polygon mode, bits 15-17
    // polygon offset enables (point, line, fill).
    void ApplyPolygon(const ReplayCommand& c) {
        if (c.raw.empty())
            return;
        const u32 w = u32(c.raw[0]);
        if (w == last_polygon_)
            return;
        last_polygon_ = w;
        s_.raster_serial++;
        const u32 cull = w & 3;
        s_.cull_enable = cull != 0;
        s_.cull_face = cull == 1 ? 0x404 : cull == 3 ? 0x408 : 0x405;
        s_.front_ccw = (w >> 2) & 1;
        s_.polygon_offset = (w >> 17) & 1;
    }

    void Draw(u32 mode, bool indexed, u32 first, u32 count, u32 index_type, u64 index_addr, s32 base_vertex,
              u32 instances, u32 base_instance) {
        if (program_constants_dirty_) {
            program_constants_dirty_ = false;
            for (u32 s : {u32(kStageVertex), u32(kStageFragment), u32(kStageGeometry)})
                if (s_.program[s])
                    Bind(kHwStage[s], 1, ProgramConstants(s_.program[s]), 0x10000);
        }
        if (indexed) {
            s_.index_addr = Va(index_addr);
            s_.index_format = index_type;
        }
        s_.base_vertex = base_vertex;
        s_.base_instance = base_instance;
        auto& d = s_.draw;
        d.topology = mode & 0xFFFF;
        d.indexed = indexed;
        d.first = first;
        d.count = count;
        Renderer* r = g_.GetRenderer();
        for (u32 k = 0; k < instances && k < 4096; k++) {
            d.instance = k;
            g_.draws++;
            if (r && (count || d.indirect_args)) {
                const u64 t0 = g_.NowNs();
                r->Draw();
                g_.draw_ns += g_.NowNs() - t0;
            }
        }
    }

    // A program's constant buffer 1 (compiler constants, BRX tables) follows its code.
    u64 ProgramConstants(u64 sph) {
        auto it = constants_.find(sph);
        if (it != constants_.end())
            return it->second;
        static thread_local std::vector<u8> buf;
        buf.assign(0x40000, 0);
        const size_t got = g_.ReadGpuPartial(sph, buf.data(), buf.size());
        const u64 c1 = common::AlignUp(sph + shader::ProgramSize(buf.data(), got), 0x100);
        constants_[sph] = c1;
        return c1;
    }

    std::unordered_map<u64, u64> constants_;
    // From a compute program's NVN control section (offsets learned against the driver's launches).
    struct ComputeInfo {
        u32 block[3] = {1, 1, 1};
        u32 shared = 0;
        u32 constants = 0, constants_size = 0; // constant buffer 1, relative to the code
    };
    std::unordered_map<u64, ComputeInfo> compute_;
    const ComputeInfo& Compute(u64 code, u64 control) {
        auto it = compute_.find(code);
        if (it != compute_.end())
            return it->second;
        ComputeInfo ci;
        u32 w[0x60]{};
        if (control && g_.Mem().IsValid(control + 0x6F0, sizeof(w))) {
            g_.Mem().ReadBlock(control + 0x6F0, w, sizeof(w));
            auto at = [&](u32 off) { return w[(off - 0x6F0) / 4]; };
            ci.block[0] = std::max<u32>(at(0x748), 1), ci.block[1] = std::max<u32>(at(0x74C), 1);
            ci.block[2] = std::max<u32>(at(0x750), 1);
            ci.shared = u32(common::AlignUp(at(0x754), 0x100));
            ci.constants = at(0x700);
            ci.constants_size = u32(common::AlignUp(at(0x6FC), 0x100));
        } else
            Unhandled("compute program without control section");
        return compute_.emplace(code, ci).first->second;
    }
    struct Ubo {
        u64 addr = 0;
        u32 size = 0;
    } cs_ubo_[5];
    u64 cs_code_ = 0, cs_control_ = 0;
    u32 row_stride_ = 0, image_stride_ = 0;
    bool program_constants_dirty_ = false;
    // The raw state words last applied: the game rebinds the same states constantly.
    u64 last_blend_[8] = {~0ull, ~0ull, ~0ull, ~0ull, ~0ull, ~0ull, ~0ull, ~0ull};
    u64 last_depth_ = ~0ull;
    u32 last_color_ = ~0u, last_mask_ = ~0u, last_polygon_ = ~0u;
    Gpu& g_;
    DrawState& s_;
    float viewport_[16][4]{};
    float depth_range_[16][2]{};
    std::set<std::string> unhandled_;
};

} // namespace

// The driver never sees the command buffer commands the encoder handles: each submit's commands are encoded and
// rendered on the GPU thread, in submission order with the host's fences and presents.
void InstallNvnEncoder(Gpu& gpu) {
    static Gpu* g = &gpu;
    static Encoder* enc = new Encoder(gpu);
    hle::nvn::SetReplaySink([](std::vector<hle::nvn::ReplaySpan>&& spans) {
        auto list = std::make_shared<Frame>(std::move(spans));
        g->SubmitHost([list] {
            static u64 jobs = 0, ns = 0, cmds = 0;
            const u64 t0 = g->NowNs();
            hle::nvn::TimelineEvent("job-start", list->size());
            enc->Run(*list);
            hle::nvn::TimelineEvent("job-end", list->size());
            ns += g->NowNs() - t0, cmds += list->size();
            if (++jobs % 600 == 0) {
                LOG_INFO("NVN", "native: {:.0f} commands per submit, {:.2f} ms each on the GPU thread; indirect draws/submit: {:.1f} host-known, {:.1f} read back, {:.1f} arrays",
                         cmds / 600.0, ns / 600e6, g_indirect_known.exchange(0) / 600.0, g_indirect_unknown.exchange(0) / 600.0,
                         g_indirect_arrays.exchange(0) / 600.0);
                ns = cmds = 0;
            }
        });
    });
    LOG_INFO("NVN", "native: command buffers are encoded and rendered by the host");
}

} // namespace gpu
