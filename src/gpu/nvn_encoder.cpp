#include "gpu/nvn_encoder.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "common/common.h"
#include "core/hle/nvn/nvn_hle.h"
#include "core/memory.h"
#include "gpu/gpu.h"
#include "gpu/maxwell3d.h"
#include "gpu/renderer.h"
#include "gpu/shader/translator.h"

namespace gpu {

using namespace reg3d;
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
    while (bh0 < 4 && (8u << bh0) < hb0)
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
// The same copy, independent of how the launch encodes it (remapped elements vs bytes).
struct CopyOp {
    u64 src = 0, dst = 0;
    u32 pitch_in = 0, pitch_out = 0, bytes = 0, lines = 0;
    bool src_bl = false, dst_bl = false;
    u32 bh = 0, bd = 0, width = 0, height = 0, depth = 0, layer = 0, ox = 0, oy = 0; // block-linear side
};
CopyOp Canonical(const u32* r, u32 launch) {
    CopyOp c;
    c.src = (u64(r[0x100] & 0xFF) << 32) | r[0x101];
    c.dst = (u64(r[0x102] & 0xFF) << 32) | r[0x103];
    c.pitch_in = r[0x104], c.pitch_out = r[0x105];
    u32 bpp = 1;
    if (launch & (1u << 10)) {
        const u32 rm = r[0x1C2];
        bpp = (((rm >> 16) & 3) + 1) * (((rm >> 24) & 3) + 1);
    }
    c.bytes = r[0x106] * bpp;
    c.lines = (launch & (1u << 9)) ? std::max<u32>(r[0x107], 1) : 1;
    c.src_bl = !(launch & (1u << 7)), c.dst_bl = !(launch & (1u << 8));
    if (c.src_bl || c.dst_bl) {
        const u32 b = c.dst_bl ? 0x1C3 : 0x1CA;
        c.bh = (r[b] >> 4) & 0xF, c.bd = (r[b] >> 8) & 0xF;
        c.width = r[b + 1] * bpp, c.height = r[b + 2], c.depth = std::max<u32>(r[b + 3], 1), c.layer = r[b + 4];
        c.ox = (r[b + 5] & 0xFFFF) * bpp, c.oy = r[b + 5] >> 16;
    }
    return c;
}

// One submit's commands: the recorder's shared lists (kept alive) and the commands in execution order.
struct Frame {
    std::vector<hle::nvn::ReplaySpan> spans;
    std::vector<const ReplayCommand*> cmds;

    explicit Frame(std::vector<hle::nvn::ReplaySpan>&& s = {}) : spans(std::move(s)) {
        for (const auto& sp : spans)
            for (size_t i = sp.begin; i < sp.end; i++)
                cmds.push_back(&(*sp.list)[i]);
    }
    void Append(Frame&& f) {
        spans.insert(spans.end(), std::make_move_iterator(f.spans.begin()), std::make_move_iterator(f.spans.end()));
        cmds.insert(cmds.end(), f.cmds.begin(), f.cmds.end());
    }
    size_t size() const { return cmds.size(); }
    const ReplayCommand& operator[](size_t i) const { return *cmds[i]; }
    struct Iter {
        const ReplayCommand* const* p;
        const ReplayCommand& operator*() const { return **p; }
        Iter& operator++() { return ++p, *this; }
        bool operator!=(const Iter& o) const { return p != o.p; }
    };
    Iter begin() const { return {cmds.data()}; }
    Iter end() const { return {cmds.data() + cmds.size()}; }
};

bool Is(std::string_view name, std::string_view suffix) {
    return name.size() >= suffix.size() && name.substr(name.size() - suffix.size()) == suffix;
}

// ---------------------------------------------------------------- snapshots (verification)
struct Snap {
    std::array<u32, 0x1000> regs;
    ConstBufferBinding cb[5][18];
    Maxwell3D::DrawState draw;
    u64 prog[6]{};
    std::vector<u32> cbdata[5][3];
    int cmd = -1;
};

std::unique_ptr<Snap> TakeSnap(Gpu& g, const Maxwell3D& m, int cmd) {
    auto s = std::make_unique<Snap>();
    s->regs = m.Regs();
    std::memcpy(s->cb, m.CBufs(), sizeof(s->cb));
    s->draw = m.draw;
    s->cmd = cmd;
    const u64 base = m.Addr(kCodeAddrHigh);
    for (u32 p = 0; p < 6; p++)
        if ((m.Reg(kShaderConfig + p * 0x10) & 1) || p == 1)
            s->prog[p] = base + m.Reg(kShaderConfig + p * 0x10 + 1);
    for (u32 st = 0; st < 5; st++)
        for (u32 c = 0; c < 3; c++)
            if (s->cb[st][c].enabled) {
                auto& d = s->cbdata[st][c];
                d.resize(std::min<u32>(s->cb[st][c].size, c == 0 ? 0x7000 : 0x400) / 4);
                g.ReadGpu(s->cb[st][c].address, d.data(), d.size() * 4);
            }
    return s;
}

// ---------------------------------------------------------------- encoder
class Encoder {
public:
    explicit Encoder(Gpu& g) : g_(g), m_(g) {
        // driver constant buffers c0-c2 of every graphics stage, and the texture handle buffer index
        // (as the driver: one c0 shared by all stages, c1 = the bound program's constants, none for tessellation)
        for (u32 hw = 0; hw < 5; hw++) {
            Bind(hw, 0, DrvCb(0, 0), 0x7000);
            Bind(hw, 2, DrvCb(hw, 2), 0x200);
            for (u32 i = 1; i < 8; i++)
                DrvWrite(hw, 2, i * 4, i);
        }
        for (u32 i = 1; i < 8; i++)
            DrvWrite(5, 2, i * 4, i);
        W(kTexCbIndex, 2);
        W(kScreenYControl, 0x10);
        W(kViewportTransformEnable, 1);
        W(0x64F, 0x181D);
        DrvWrite(0, 0, 0x610, 0x44FFFE00u); // driver constant (seen in every draw)
        ResetAttribs();
    }
    Maxwell3D& M() { return m_; }
    int current = -1; // command being encoded (snapshots)
    std::map<std::string, u64> unhandled;
    u64 no_shadow = 0;

    void Run(const Frame& cmds) {
        for (size_t i = 0; i < cmds.size(); i++) {
            current = int(i);
            Encode(cmds[i]);
        }
        current = -1;
    }

    // ---- native replay in step with the command stream (MUA3_NVN_REPLAY): submits are queued by the game thread and
    // encoded on the GPU thread, one draw command at a time.
    void Queue(Frame&& cmds) {
        bool draws = false;
        for (const auto& c : cmds)
            draws |= c.name.find("CommandBufferDraw") != std::string_view::npos;
        std::lock_guard lk(queue_mutex_);
        // submits without draws (uploads while loading) join the previous one: only draws drain the queue
        if (!draws && !queue_.empty()) {
            queue_.back().Append(std::move(cmds));
            return;
        }
        queue_.push_back(std::move(cmds));
        if (queue_.size() > 256)
            queue_.pop_front(), dropped++;
    }
    // Encodes the commands up to the next draw command (clears included). False if nothing is queued.
    bool ToDraw() {
        for (;;) {
            if (pos_ >= frame_.size()) {
                std::lock_guard lk(queue_mutex_);
                if (queue_.empty())
                    return false;
                frame_ = std::move(queue_.front());
                queue_.pop_front();
                pos_ = 0;
                continue;
            }
            const ReplayCommand& c = frame_[pos_];
            if (c.name.find("CommandBufferDraw") != std::string_view::npos || c.name.ends_with("DispatchCompute"))
                return true;
            Encode(c);
            pos_++;
        }
    }
    // Programs of the pending draw (vertex, fragment Maxwell program header addresses).
    bool PendingDispatch() const { return frame_[pos_].name.ends_with("DispatchCompute"); }
    // Program and grid of the pending dispatch.
    u64 DispatchProgram() const { return cs_code_; }
    u32 DispatchGrid(u32 i) const { return u32(frame_[pos_].x[1 + i]); }
    u64 DrawVs() const { return prog_[0]; }
    u64 DrawFs() const { return prog_[1]; }
    // Encodes the pending draw command; returns the hardware draws (instances) it issued.
    u32 RunDraw() {
        instances_ = 0;
        Encode(frame_[pos_++]);
        return instances_;
    }
    // The pending draw command issues no hardware draw (indirect with zero instances).
    bool PendingIssuesNothing() {
        const ReplayCommand& c = frame_[pos_];
        if (c.name.ends_with("DispatchCompute"))
            return c.x[1] == 0 || c.x[2] == 0 || c.x[3] == 0;
        u32 a[2]{};
        if (c.name.ends_with("DrawElementsIndirect"))
            SyncForRead(c.x[4], sizeof(a)), g_.ReadGpu(c.x[4], a, sizeof(a));
        else if (c.name.ends_with("DrawArraysIndirect"))
            SyncForRead(c.x[2], sizeof(a)), g_.ReadGpu(c.x[2], a, sizeof(a));
        else
            return false;
        return a[1] == 0;
    }
    // Skips the rest of the current submit without rendering (after losing step with the stream).
    void Discard() {
        m_.capture = [](const Maxwell3D&) {};
        while (pos_ < frame_.size())
            Encode(frame_[pos_++]);
        m_.capture = nullptr;
    }
    u64 dropped = 0;

private:
    void W(u32 r, u32 v) { m_.CallMethod(r, v); }
    void WA(u32 hi, u64 a) {
        W(hi, u32(a >> 32));
        W(hi + 1, u32(a));
    }
    void Bind(u32 hw, u32 index, u64 addr, u32 size) {
        W(kCbSize, size);
        WA(kCbAddrHigh, addr);
        W(kCbBind + hw * 8, (index << 4) | (addr && size ? 1 : 0));
    }
    template <class T>
    void DrvWrite(u32 hw, u32 cb, u32 offset, const T& v) {
        if (offset + sizeof(T) <= kDrvCbSize)
            std::memcpy(g_.HostMem() + (DrvCb(hw, cb) - Gpu::kHostBase) + offset, &v, sizeof(T));
    }

    void Encode(const ReplayCommand& c) {
        const std::string_view n = c.name.substr(c.name.find("CommandBuffer") == 3 ? 16 : 0);
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
            if (x[1] < 16)
                streams_[x[1]].addr = x[2], streams_[x[1]].size = x[3];
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
                        viewport_[k][j] = Raw32(c, i * 4 + j);
                } else {
                    depth_range_[k][0] = Raw32(c, i * 2);
                    depth_range_[k][1] = Raw32(c, i * 2 + 1);
                }
                WriteViewport(k);
            }
        } else if (n == "SetScissors") {
            for (u32 i = 0; i < u32(x[2]) && x[1] + i < 16; i++)
                SetScissor(u32(x[1]) + i, Raw32(c, i * 4), Raw32(c, i * 4 + 1), Raw32(c, i * 4 + 2), Raw32(c, i * 4 + 3));
        } else if (n == "SetScissor")
            SetScissor(0, u32(x[1]), u32(x[2]), u32(x[3]), u32(x[4]));
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
                 n == "SetShaderScratchMemory" || n == "ReportCounter" || n == "SetRenderEnable" ||
                 n == "SetRenderEnableConditional") {
            // no renderer state (render enables: the emulated path draws unconditionally too)
        } else if (n == "SetStencilRef" || n == "SetStencilValueMask" || n == "SetStencilMask") {
            const u32 front = n == "SetStencilRef" ? kStencilFrontRef : n == "SetStencilMask" ? kStencilFrontMask : kStencilFrontFuncMask;
            const u32 back = n == "SetStencilRef" ? 0x3D5u : n == "SetStencilMask" ? 0x3D6u : 0x3D7u;
            if (x[1] & 1)
                W(front, u32(x[2]));
            if (x[1] & 2)
                W(back, u32(x[2]));
        } else if (n == "SetPolygonOffsetClamp") {
            W(kSlopeScaleDepthBias, c.v[0]);
            W(kDepthBias, F2U(U2F(c.v[1]) * 2));
            W(kDepthBiasClamp, c.v[2]);
        } else if (n == "SetPrimitiveRestart") {
            W(0x591, u32(x[1]) & 1);
            W(0x592, x[1] & 1 ? u32(x[2]) : 0);
        } else if (n == "SetTexturePool" || n == "SetSamplerPool") {
            const u64 base = hle::nvn::QueryTexturePool(x[1]);
            const bool tex = n == "SetTexturePool";
            WA(tex ? kTicAddrHigh : kTscAddrHigh, base);
        } else if (n == "ClearColor") {
            for (u32 j = 0; j < 4; j++)
                W(kClearColor + j, Raw32(c, j));
            W(kClearBuffers, (u32(x[1]) << 6) | ((u32(x[3]) & 0xF) << 2));
        } else if (n == "ClearDepthStencil") {
            W(kClearDepth, c.v[0]);
            W(kClearStencil, u32(x[2]));
            W(kClearBuffers, (x[1] ? 1u : 0u) | (x[3] ? 2u : 0u));
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
            Copy(l);
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
            SyncForRead(x[4], sizeof(a));
            g_.ReadGpu(x[4], a, sizeof(a));
            Draw(u32(x[1]), true, a[2], a[0], u32(x[2]), x[3], s32(a[3]), a[1], a[4]);
        } else if (n == "DrawArraysIndirect") {
            u32 a[4]{}; // count, instances, first, base instance
            SyncForRead(x[2], sizeof(a));
            g_.ReadGpu(x[2], a, sizeof(a));
            Draw(u32(x[1]), false, a[2], a[0], 0, 0, 0, a[1], a[3]);
        } else
            unhandled[std::string(n)]++;
    }

    // Indirect arguments are often written by compute just before: land those results first (rendering modes).
    void SyncForRead(u64 addr, u64 size) {
        if (render_compute && g_.GetRenderer())
            g_.GetRenderer()->SyncRegion(addr, size);
    }

    u32 Raw32(const ReplayCommand& c, u32 i) const {
        return i / 2 < c.raw.size() ? u32(c.raw[i / 2] >> ((i & 1) * 32)) : 0;
    }

    void BindProgram(const ReplayCommand& c) {
        u64 code = 0, control = 0;
        if (c.x[1]) {
            if (!c.objects.empty())
                for (const StateCall& k : c.objects[0])
                    if (Is(k.name, "SetShaders") && k.handles.size() >= 2)
                        code = k.handles[0], control = k.handles[1];
            if (!code)
                code = hle::nvn::QueryProgramCode(c.x[1]), no_shadow++;
        }
        for (u32 s = 0; s < 6; s++)
            if (c.x[2] & (1u << s))
                prog_[s] = code ? code + 0x30 : 0; // the Maxwell program header follows NVN's 0x30-byte blob header
        if (c.x[2] & 0x20)
            cs_code_ = code, cs_control_ = control;
    }

    // Texture <-> buffer copies, one DMA launch per slice / layer: linear rows of whole blocks on the buffer side.
    void CopyTexture(const ReplayCommand& c, bool upload) {
        const u64 buffer = upload ? c.x[1] : c.x[4], tex = upload ? c.x[2] : c.x[1];
        hle::nvn::TextureInfo t;
        if (!hle::nvn::QueryTexture(tex, t)) {
            unhandled["copy: texture not in the model"]++;
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
                unhandled[fmt::format("copy: layout of format {:#x} target {}", t.format, t.target)]++;
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
            Copy(l);
        }
    }
    void Copy(const DmaLaunch& l) {
        if (on_copy)
            on_copy(l);
        else if (render_compute)
            g_.RunDma(l.r.data(), l.launch);
    }

  public:
    std::function<void(const DmaLaunch&)> on_copy; // verification
    // Compute launch as the renderer takes it: compute engine registers and launch descriptor (QMD).
    struct Launch {
        std::array<u32, 0x1000> r{};
        std::array<u32, 0x40> q{};
        u64 code = 0, control = 0; // NVN shader data of the program
    };
    std::function<void(const Launch&)> on_dispatch; // verification
    bool render_compute = false;                    // replay: launch on the renderer
  private:
    void Dispatch(u32 gx, u32 gy, u32 gz) {
        static thread_local Launch l;
        l.r.fill(0);
        l.q.fill(0);
        l.code = cs_code_, l.control = cs_control_;
        // Compute code has no blob header; the launch shape and constants come from NVN's control section.
        const ComputeInfo& ci = Compute(cs_code_, cs_control_);
        const u64 prog = cs_code_;
        const u64 base = prog & ~0xFFFFull;
        l.r[0x582] = u32(base >> 32), l.r[0x583] = u32(base);
        l.r[0x55D] = m_.Reg(kTicAddrHigh), l.r[0x55E] = m_.Reg(kTicAddrLow), l.r[0x55F] = m_.Reg(kTicLimit);
        l.r[0x557] = m_.Reg(kTscAddrHigh), l.r[0x558] = m_.Reg(kTscAddrLow), l.r[0x559] = m_.Reg(kTscLimit);
        l.r[0x982] = 2;
        l.q[8] = u32(prog - base);
        l.q[0xC] = gx;
        l.q[0xD] = gy | (gz << 16);
        l.q[0x11] = ci.shared;
        l.q[0x12] = ci.block[0] << 16;
        l.q[0x13] = ci.block[1] | (ci.block[2] << 16);
        DrvWrite(5, 0, 0x200, gx);
        DrvWrite(5, 0, 0x204, gy);
        DrvWrite(5, 0, 0x208, gz);
        u32 mask = 0;
        auto slot = [&](u32 i, u64 addr, u32 size) {
            if (!addr || !size)
                return;
            mask |= 1u << i;
            l.q[0x1D + i * 2] = u32(addr);
            l.q[0x1E + i * 2] = (u32(addr >> 32) & 0xFF) | (std::min<u32>(size, 0x10000) << 15);
        };
        slot(0, DrvCb(5, 0), 0x1000);
        slot(1, ci.constants_size ? cs_code_ + ci.constants : 0, ci.constants_size);
        slot(2, DrvCb(5, 2), 0x600);
        for (u32 i = 0; i < 5; i++)
            slot(3 + i, cs_ubo_[i].addr, cs_ubo_[i].size);
        l.q[0x14] = mask | 0x60000000;
        if (on_dispatch)
            on_dispatch(l);
        else if (render_compute && g_.GetRenderer())
            g_.GetRenderer()->Dispatch(l.r.data(), l.q.data());
    }

    void ResetAttribs() {
        for (u32 i = 0; i < 32; i++)
            attribs_[i] = i < 16 ? 0x3A400040u : 0; // the driver's "constant input" value
    }

    void BindAttribs(const ReplayCommand& c) {
        const u32 count = std::min<u32>(u32(c.x[1]), 32);
        ResetAttribs();
        for (u32 i = 0; i < count && i < c.objects.size(); i++) {
            u32 fmt = 0, offset = 0, stream = 0;
            if (c.objects[i].empty())
                no_shadow++;
            for (const StateCall& k : c.objects[i]) {
                if (Is(k.name, "SetFormat"))
                    fmt = u32(k.x[1]), offset = u32(k.x[2]);
                else if (Is(k.name, "SetStreamIndex"))
                    stream = u32(k.x[1]);
            }
            const u32 hw = AttribFormat(fmt);
            if (!hw) {
                if (fmt)
                    unhandled[fmt::format("vertex format {:#x}", fmt)]++;
                continue;
            }
            attribs_[i] = (stream & 0x1F) | ((offset & 0x3FFF) << 7) | hw;
        }
    }

    void BindStreams(const ReplayCommand& c) {
        const u32 count = std::min<u32>(u32(c.x[1]), 16);
        for (u32 i = 0; i < count && i < c.objects.size(); i++) {
            u32 stride = 0, divisor = 0;
            if (c.objects[i].empty())
                no_shadow++;
            for (const StateCall& k : c.objects[i]) {
                if (Is(k.name, "SetStride"))
                    stride = u32(k.x[1]);
                else if (Is(k.name, "SetDivisor"))
                    divisor = u32(k.x[1]);
            }
            streams_[i].stride = stride, streams_[i].divisor = divisor;
        }
    }

    void SetRenderTargets(const ReplayCommand& c) {
        const u32 count = std::min<u32>(u32(c.x[1]), 8);
        u32 rtc = count | 0xFAC6880; // identity slot map, as the driver writes it
        for (u32 i = 0; i < 8; i++) {
            const u32 base = kRenderTarget + i * 0x10;
            hle::nvn::TextureInfo t;
            if (i >= count || i >= c.extra.size() || !c.extra[i] || !hle::nvn::QueryTexture(c.extra[i], t)) {
                W(base + 4, 0);
                continue;
            }
            auto f = kNvnRtFormat.find(t.format);
            if (f == kNvnRtFormat.end())
                unhandled[fmt::format("rt format {:#x}", t.format)]++;
            WA(base, t.gpu);
            W(base + 2, t.width);
            W(base + 3, t.height);
            W(base + 4, f == kNvnRtFormat.end() ? 0 : f->second);
            W(base + 5, TileMode(t.height));
            W(base + 6, std::max<u32>(t.depth, 1));
        }
        W(kRtControl, rtc);
        hle::nvn::TextureInfo d;
        if (c.x[4] && hle::nvn::QueryTexture(c.x[4], d)) {
            auto f = kNvnZetaFormat.find(d.format);
            if (f == kNvnZetaFormat.end())
                unhandled[fmt::format("depth format {:#x}", d.format)]++;
            WA(kZetaAddrHigh, d.gpu);
            W(kZetaFormat, f == kNvnZetaFormat.end() ? 0 : f->second);
            W(kZetaTileMode, TileMode(d.height));
            W(kZetaWidth, d.width);
            W(kZetaHeight, d.height);
            W(kZetaDepth, std::max<u32>(d.depth, 1));
            W(kZetaEnable, 1);
        } else
            W(kZetaEnable, 0);
    }

    void WriteViewport(u32 i) {
        const float x = U2F(viewport_[i][0]), y = U2F(viewport_[i][1]), w = U2F(viewport_[i][2]), h = U2F(viewport_[i][3]);
        const float n = U2F(depth_range_[i][0]), f = U2F(depth_range_[i][1]);
        const u32 t = kViewportTransform + i * 8;
        W(t + 0, F2U(w * 0.5f));
        W(t + 1, F2U(-h * 0.5f));
        W(t + 2, F2U((f - n) * 0.5f));
        W(t + 3, F2U(x + w * 0.5f));
        W(t + 4, F2U(y + h * 0.5f));
        W(t + 5, F2U((f + n) * 0.5f));
        const u32 v = kViewport + i * 4;
        W(v + 0, (u32(x) & 0xFFFF) | (u32(w) << 16));
        W(v + 1, (u32(y) & 0xFFFF) | (u32(h) << 16));
        W(v + 2, depth_range_[i][0]);
        W(v + 3, depth_range_[i][1]);
    }

    void SetScissor(u32 i, u32 x, u32 y, u32 w, u32 h) {
        W(0x380 + i * 4, 1);
        W(0x381 + i * 4, (x & 0xFFFF) | ((x + w) << 16));
        W(0x382 + i * 4, (y & 0xFFFF) | ((y + h) << 16));
    }

    const std::vector<StateCall>* Object(const ReplayCommand& c) {
        if (c.objects.empty() || c.objects[0].empty()) {
            no_shadow++;
            return nullptr;
        }
        return &c.objects[0];
    }

    void ApplyBlend(const ReplayCommand& c) {
        auto* o = Object(c);
        if (!o)
            return;
        u32 target = 0, src = 2, dst = 1, src_a = 2, dst_a = 1, eq = 1, eq_a = 1;
        for (const StateCall& k : *o) {
            if (Is(k.name, "SetBlendTarget"))
                target = u32(k.x[1]) & 7;
            else if (Is(k.name, "SetBlendFunc"))
                src = u32(k.x[1]), dst = u32(k.x[2]), src_a = u32(k.x[3]), dst_a = u32(k.x[4]);
            else if (Is(k.name, "SetBlendEquation"))
                eq = u32(k.x[1]), eq_a = u32(k.x[2]);
        }
        const u32 b = kIndependentBlend + target * 8;
        W(b + 0, 0);
        W(b + 1, eq), W(b + 2, src), W(b + 3, dst), W(b + 4, eq_a), W(b + 5, src_a), W(b + 6, dst_a);
        W(0x4B9, 1);
    }

    void ApplyColor(const ReplayCommand& c) {
        auto* o = Object(c);
        if (!o)
            return;
        u32 enable[8]{};
        u32 alpha_func = 8;
        for (const StateCall& k : *o) {
            if (Is(k.name, "SetBlendEnable") && k.x[1] < 8)
                enable[k.x[1]] = u32(k.x[2]) & 1;
            else if (Is(k.name, "SetAlphaTest"))
                alpha_func = u32(k.x[1]);
        }
        for (u32 i = 0; i < 8; i++)
            W(kBlendEnable + i, enable[i]);
        W(kAlphaTestEnable, alpha_func != 8 ? 1 : 0);
        W(kAlphaTestFunc, alpha_func != 8 ? alpha_func : 0);
    }

    void ApplyChannelMask(const ReplayCommand& c) {
        auto* o = Object(c);
        if (!o)
            return;
        u32 mask[8];
        std::fill(std::begin(mask), std::end(mask), 0x1111u);
        for (const StateCall& k : *o)
            if (Is(k.name, "SetChannelMask") && k.x[1] < 8)
                mask[k.x[1]] = (k.x[2] ? 1 : 0) | (k.x[3] ? 0x10 : 0) | (k.x[4] ? 0x100 : 0) | (k.x[5] ? 0x1000 : 0);
        for (u32 i = 0; i < 8; i++)
            W(kColorMask + i, mask[i]);
    }

    void ApplyDepthStencil(const ReplayCommand& c) {
        auto* o = Object(c);
        if (!o)
            return;
        u32 test = 0, write = 1, func = 2, stencil = 0;
        u32 sfunc[2] = {8, 8}, fail[2] = {1, 1}, zfail[2] = {1, 1}, zpass[2] = {1, 1};
        for (const StateCall& k : *o) {
            if (Is(k.name, "SetDepthTestEnable"))
                test = u32(k.x[1]) & 1;
            else if (Is(k.name, "SetDepthWriteEnable"))
                write = u32(k.x[1]) & 1;
            else if (Is(k.name, "SetDepthFunc"))
                func = u32(k.x[1]);
            else if (Is(k.name, "SetStencilTestEnable"))
                stencil = u32(k.x[1]) & 1;
            else if (Is(k.name, "SetStencilFunc")) {
                for (u32 f = 0; f < 2; f++)
                    if (k.x[1] & (1u << f))
                        sfunc[f] = u32(k.x[2]);
            } else if (Is(k.name, "SetStencilOp")) {
                for (u32 f = 0; f < 2; f++)
                    if (k.x[1] & (1u << f))
                        fail[f] = u32(k.x[2]), zfail[f] = u32(k.x[3]), zpass[f] = u32(k.x[4]);
            }
        }
        W(kDepthTestEnable, test);
        W(kDepthWriteEnable, write);
        W(kDepthTestFunc, func);
        W(kStencilEnable, stencil);
        W(kStencilFrontFunc, sfunc[0]);
        W(kStencilFrontOpFail, fail[0]), W(kStencilFrontOpZFail, zfail[0]), W(kStencilFrontOpZPass, zpass[0]);
        W(kStencilTwoSide, 1);
        W(kStencilBackFunc, sfunc[1]);
        W(kStencilBackOpFail, fail[1]), W(kStencilBackOpZFail, zfail[1]), W(kStencilBackOpZPass, zpass[1]);
    }

    void ApplyPolygon(const ReplayCommand& c) {
        auto* o = Object(c);
        if (!o)
            return;
        u32 cull = 0, front = 1, offsets = 0;
        for (const StateCall& k : *o) {
            if (Is(k.name, "SetCullFace"))
                cull = u32(k.x[1]);
            else if (Is(k.name, "SetFrontFace"))
                front = u32(k.x[1]);
            else if (Is(k.name, "SetPolygonOffsetEnables"))
                offsets = u32(k.x[1]);
        }
        W(kCullEnable, cull ? 1 : 0);
        W(kCullFace, cull == 1 ? 0x404 : cull == 3 ? 0x408 : 0x405);
        W(kFrontFace, front ? 0x901 : 0x900);
        W(kPolygonOffsetFillEnable, (offsets >> 2) & 1);
    }

    void Draw(u32 mode, bool indexed, u32 first, u32 count, u32 index_type, u64 index_addr, s32 base_vertex,
              u32 instances, u32 base_instance) {
        // programs: one code base for this draw's stages
        u64 base = ~0ull;
        for (u32 s = 0; s < 5; s++)
            if (prog_[s])
                base = std::min(base, prog_[s] & ~0xFFull);
        if (base == ~0ull)
            base = 0;
        WA(kCodeAddrHigh, base);
        u64 slot[6]{};
        for (u32 s = 0; s < 5; s++)
            if (prog_[s])
                slot[kHwProgram[s]] = prog_[s];
        for (u32 p = 0; p < 6; p++) {
            W(kShaderConfig + p * 0x10, slot[p] ? 1 | (p << 4) : 0);
            W(kShaderConfig + p * 0x10 + 1, slot[p] ? u32(slot[p] - base) : 0);
        }
        for (u32 s : {0u, 1u, 2u})
            if (prog_[s])
                Bind(kHwStage[s], 1, ProgramConstants(prog_[s]), 0x10000);
        // vertex input
        for (u32 i = 0; i < 16; i++) {
            const Stream& st = streams_[i];
            W(kVertexArray + i * 4, (st.stride & 0xFFF) | (st.addr ? 0x1000 : 0));
            WA(kVertexArray + i * 4 + 1, st.addr);
            W(kVertexArray + i * 4 + 3, st.divisor);
            WA(kVertexArrayLimit + i * 2, st.addr + (st.size ? st.size - 1 : 0));
        }
        for (u32 i = 0; i < 32; i++)
            W(kVertexAttribFormat + i, attribs_[i]);
        // indices
        if (indexed) {
            const u32 size = index_type == 0 ? 1 : index_type == 1 ? 2 : 4;
            WA(kIndexAddrHigh, index_addr);
            WA(kIndexLimitHigh, index_addr + u64(first + count) * size - 1);
            W(kIndexFormat, index_type);
            W(kIndexFirst, first);
        } else {
            W(kVertexBufferFirst, first);
            W(kVertexBufferCount, count);
        }
        W(0x50D, u32(base_vertex));
        W(0x50E, base_instance);
        for (u32 k = 0; k < instances && k < 4096; k++) {
            if (indexed)
                W(kIndexCount, count);
            W(kDrawBegin, mode | (k ? 1u << 26 : 0));
            instances_++;
            W(kDrawEnd, 0);
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
            unhandled["compute program without control section"]++;
        return compute_.emplace(code, ci).first->second;
    }
    struct Ubo {
        u64 addr = 0;
        u32 size = 0;
    } cs_ubo_[5];
    u64 cs_code_ = 0, cs_control_ = 0;
    u32 row_stride_ = 0, image_stride_ = 0;
    std::mutex queue_mutex_;
    std::deque<Frame> queue_;
    Frame frame_;
    size_t pos_ = 0;
    u32 instances_ = 0;
    struct Stream {
        u32 stride = 0, divisor = 0;
        u64 addr = 0, size = 0;
    };
    Gpu& g_;
    Maxwell3D m_;
    u64 prog_[6]{}; // NVN stage -> Maxwell program header address
    Stream streams_[16];
    u32 attribs_[32];
    u32 viewport_[16][4]{};
    u32 depth_range_[16][2]{};
};

// ---------------------------------------------------------------- verification
struct Verify {
    Gpu* gpu = nullptr;
    std::unique_ptr<Encoder> enc;
    u64 at_ms = 0;
    std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
    std::atomic<int> phase{0}; // 0 waiting, 1 encoded (waiting for the driver's draws), 2 collecting, 3 done
    std::vector<std::unique_ptr<Snap>> enc_snaps, drv_snaps;
    struct CSnap {
        u32 r[8]; // code base hi/lo, TIC hi/lo, TSC hi/lo, texture cbuf
        u32 q[0x40];
        std::vector<u32> cb[3];
        u64 code = 0, control = 0;
        std::vector<u32> control_words;
        int cmd = -1;
    };
    std::vector<CSnap> enc_cs, drv_cs;
    std::vector<CopyOp> enc_copy, drv_copy;
    std::vector<int> enc_copy_cmd;
    std::chrono::steady_clock::time_point collect_start;
    bool copies_mode = std::getenv("MUA3_NVN_VERIFY_COPIES") != nullptr; // capture a submit with copies
    std::vector<std::string> cmd_names, cmd_lines;
    std::map<std::string, u64> unhandled; // encoder diagnostics as of the captured frame
    u64 no_shadow = 0;
    size_t want = 0;
};
Verify& V() {
    static Verify v;
    return v;
}

std::string RegName(u32 r) {
    struct N {
        u32 first, count, stride;
        const char* name;
    };
    static const N names[] = {
        {kRenderTarget, 8, 0x10, "rt"}, {kViewportTransform, 1, 8, "vptransform"}, {kViewport, 1, 4, "viewport"},
        {0x380, 1, 4, "scissor"}, {kVertexAttribFormat, 32, 1, "attrib"}, {kVertexArray, 16, 4, "varray"},
        {kVertexArrayLimit, 16, 2, "vlimit"}, {kIndependentBlend, 8, 8, "iblend"}, {kColorMask, 8, 1, "colormask"},
        {kBlendEnable, 8, 1, "blendenable"}};
    for (const N& n : names)
        if (r >= n.first && r < n.first + n.count * n.stride)
            return fmt::format("{}{}+{}({:#x})", n.name, (r - n.first) / n.stride, (r - n.first) % n.stride, r);
    return fmt::format("reg{:#x}", r);
}

Verify::CSnap TakeCSnap(Gpu& g, const u32* r, const u32* q, int cmd) {
    Verify::CSnap c;
    const u32 rr[8] = {r[0x582], r[0x583], r[0x55D], r[0x55E], r[0x557], r[0x558], r[0x982], 0};
    std::memcpy(c.r, rr, sizeof(rr));
    std::memcpy(c.q, q, sizeof(c.q));
    c.cmd = cmd;
    for (u32 i = 0; i < 3; i++)
        if (q[0x14] & (1u << i)) {
            const u64 a = (u64(q[0x1E + i * 2] & 0xFF) << 32) | q[0x1D + i * 2];
            c.cb[i].resize(std::min<u32>(q[0x1E + i * 2] >> 15, i == 0 ? 0x1000u : 0x400u) / 4);
            g.ReadGpu(a, c.cb[i].data(), c.cb[i].size() * 4);
        }
    return c;
}

void ReportCopies(std::ofstream& f) {
    auto& v = V();
    const auto& E = v.enc_copy;
    const auto& D = v.drv_copy;
    std::ofstream log("nvn_verify_copies.txt");
    std::map<std::string, std::pair<u64, std::vector<std::string>>> diffs;
    auto add = [&](const std::string& k, size_t i, u64 a, u64 b) {
        auto& d = diffs[k];
        d.first++;
        if (d.second.size() < 4)
            d.second.push_back(fmt::format("k{} drv {:#x} enc {:#x}", i, a, b));
    };
    auto line = [](const CopyOp& c) {
        return fmt::format("src {:#x}{} dst {:#x}{} pitch {}/{} bytes {} lines {} bl bh {} bd {} w {} h {} d {} layer {} o {},{}", c.src,
                           c.src_bl ? "(bl)" : "", c.dst, c.dst_bl ? "(bl)" : "", c.pitch_in, c.pitch_out, c.bytes, c.lines, c.bh, c.bd,
                           c.width, c.height, c.depth, c.layer, c.ox, c.oy);
    };
    size_t cursor = 0, matched = 0;
    for (size_t i = 0; i < E.size(); i++) {
        const auto& e = E[i];
        size_t j = cursor;
        for (; j < D.size() && j < cursor + 16; j++)
            if (D[j].src_bl == e.src_bl && D[j].dst_bl == e.dst_bl && (e.dst_bl ? D[j].src == e.src : D[j].dst == e.dst))
                break;
        if (j >= D.size() || j >= cursor + 16) {
            log << fmt::format("enc {} (cmd {}): no match: {}\n", i, v.enc_copy_cmd[i], line(e));
            add("unmatched", i, 0, 0);
            continue;
        }
        cursor = j + 1, matched++;
        const auto& d = D[j];
        log << fmt::format("enc {} (cmd {}) {}\n      drv {}: {}\n", i, v.enc_copy_cmd[i], line(e), j, line(d));
#define CMP(field) \
    if (d.field != e.field) \
        add(#field, i, d.field, e.field);
        CMP(src) CMP(dst) CMP(pitch_in) CMP(pitch_out) CMP(bytes) CMP(lines) CMP(bh) CMP(bd) CMP(width) CMP(height) CMP(depth)
        CMP(layer) CMP(ox) CMP(oy)
#undef CMP
    }
    f << fmt::format("# copies: encoder {} driver {} matched {}\n", E.size(), D.size(), matched);
    for (const auto& [k, d] : diffs) {
        f << fmt::format("{:<28} {:>6}  ", k, d.first);
        for (const auto& ex : d.second)
            f << ex << "; ";
        f << "\n";
    }
}

void ReportCompute(std::ofstream& f) {
    auto& v = V();
    const auto& E = v.enc_cs;
    const auto& D = v.drv_cs;
    std::ofstream log("nvn_verify_compute.txt");
    std::map<std::string, std::pair<u64, std::vector<std::string>>> diffs;
    auto add = [&](const std::string& k, size_t i, u64 a, u64 b) {
        auto& d = diffs[k];
        d.first++;
        if (d.second.size() < 4)
            d.second.push_back(fmt::format("c{} drv {:#x} enc {:#x}", i, a, b));
    };
    auto abs_prog = [](const Verify::CSnap& c) { return ((u64(c.r[0] & 0xFF) << 32) | c.r[1]) + c.q[8]; };
    size_t cursor = 0, matched = 0;
    for (size_t i = 0; i < E.size(); i++) {
        const auto& e = E[i];
        size_t j = cursor;
        for (; j < D.size() && j < cursor + 32; j++)
            if (D[j].q[0xC] == e.q[0xC] && D[j].q[0xD] == e.q[0xD] &&
                (abs_prog(D[j]) == abs_prog(e) || abs_prog(D[j]) - e.code < 0x100))
                break;
        if (j >= D.size() || j >= cursor + 32) {
            log << fmt::format("enc cs {} (cmd {}) code {:#x} grid {}x{}: no driver match\n", i, e.cmd, e.code, e.q[0xC], e.q[0xD]);
            continue;
        }
        cursor = j + 1, matched++;
        const auto& d = D[j];
        log << fmt::format("enc cs {} = drv {} (cmd {}) code {:#x} drv prog {:#x} (code+{:#x}) block {}x{}x{} shared {:#x} mask {:#x}",
                           i, j, e.cmd, e.code, abs_prog(d), abs_prog(d) - e.code, d.q[0x12] >> 16, d.q[0x13] & 0xFFFF,
                           d.q[0x13] >> 16, d.q[0x11] & 0x3FFFF, d.q[0x14]);
        for (u32 k = 0; k < 8; k++)
            if (d.q[0x14] & (1u << k))
                log << fmt::format(" c{}={:#x}/{:#x}", k, (u64(d.q[0x1E + k * 2] & 0xFF) << 32) | d.q[0x1D + k * 2], d.q[0x1E + k * 2] >> 15);
        log << " control";
        for (size_t w = 0; w < e.control_words.size(); w++)
            if (e.control_words[w])
                log << fmt::format(" [{:x}]={:x}", w * 4, e.control_words[w]);
        log << "\n";
        if (abs_prog(d) != abs_prog(e))
            add("program", i, abs_prog(d), abs_prog(e));
        for (u32 k = 2; k < 7; k++)
            if (d.r[k] != e.r[k])
                add(fmt::format("reg{}", k), i, d.r[k], e.r[k]);
        for (u32 w = 0; w < 0x40; w++) {
            if (w == 8 || (w >= 0x1D && w < 0x2D && !((d.q[0x14] >> ((w - 0x1D) / 2)) & 1) && !((e.q[0x14] >> ((w - 0x1D) / 2)) & 1)))
                continue;
            if (w >= 0x1D && w < 0x23) // driver constant buffers: compared by content
                continue;
            if (d.q[w] != e.q[w])
                add(fmt::format("qmd[{:#x}]", w), i, d.q[w], e.q[w]);
        }
        for (u32 k = 0; k < 3; k++)
            for (size_t w = 0; w < std::max(d.cb[k].size(), e.cb[k].size()); w++) {
                const u32 a = w < d.cb[k].size() ? d.cb[k][w] : 0, b = w < e.cb[k].size() ? e.cb[k][w] : 0;
                if (a != b)
                    add(fmt::format("cs c{}[{:#05x}]", k, w * 4), i, a, b);
            }
    }
    f << fmt::format("# compute: encoder {} driver {} matched {}\n", E.size(), D.size(), matched);
    for (const auto& [k, d] : diffs) {
        f << fmt::format("{:<28} {:>6}  ", k, d.first);
        for (const auto& ex : d.second)
            f << ex << "; ";
        f << "\n";
    }
}

void Report() {
    auto& v = V();
    std::ofstream f("nvn_verify.txt");
    const auto& E = v.enc_snaps;
    const auto& D = v.drv_snaps;
    f << fmt::format("# encoder draws {}, driver draws {}\n", E.size(), D.size());
    struct Diff {
        u64 count = 0;
        std::vector<std::string> examples;
    };
    std::map<std::string, Diff> diffs;
    u64 compared = 0, matched = 0;
    auto add = [&](const std::string& key, size_t draw, u64 drv, u64 enc) {
        auto& d = diffs[key];
        d.count++;
        if (d.examples.size() < 4)
            d.examples.push_back(fmt::format("d{} drv {:#x} enc {:#x}", draw, drv, enc));
    };
    size_t cursor = 0;
    std::ofstream log("nvn_verify_draws.txt");
    {
        std::ofstream cl("nvn_verify_cmds.txt");
        for (const auto& l : v.cmd_lines)
            cl << l << "\n";
    }
    for (size_t i = 0; i < E.size(); i++) {
        const Snap& e = *E[i];
        size_t j = cursor;
        for (; j < D.size() && j < cursor + 64; j++)
            if (D[j]->prog[1] == e.prog[1] && D[j]->prog[5] == e.prog[5])
                break;
        if (j >= D.size() || j >= cursor + 64) {
            log << fmt::format("enc {} (cmd {} {}) vs {:#x} fs {:#x}: no driver match\n", i, e.cmd,
                               e.cmd >= 0 ? v.cmd_names[e.cmd] : "", e.prog[1], e.prog[5]);
            continue;
        }
        cursor = j + 1;
        matched++;
        const Snap& d = *D[j];
        log << fmt::format("enc {} = drv {} (cmd {} {}) vs {:#x} fs {:#x} gs {:#x}", i, j, e.cmd,
                           e.cmd >= 0 ? v.cmd_names[e.cmd] : "", e.prog[1], e.prog[5], e.prog[4]);
        for (u32 st = 0; st < 5; st++)
            for (u32 c = 0; c < 18; c++)
                if (d.cb[st][c].enabled)
                    log << fmt::format(" s{}c{}={:#x}/{:#x}", st, c, d.cb[st][c].address, d.cb[st][c].size);
        log << "\n";
        auto cmp = [&](u32 r) {
            compared++;
            if (d.regs[r] != e.regs[r])
                add(RegName(r), i, d.regs[r], e.regs[r]);
        };
        // render targets
        const u32 rtc = d.regs[kRtControl];
        cmp(kRtControl);
        for (u32 k = 0; k < (rtc & 0xF) && k < 8; k++)
            for (u32 o = 0; o < 7; o++)
                cmp(kRenderTarget + k * 0x10 + o);
        cmp(kZetaEnable);
        if (d.regs[kZetaEnable] & 1)
            for (u32 r : {kZetaAddrHigh, kZetaAddrLow, kZetaFormat, kZetaTileMode, kZetaWidth, kZetaHeight, kZetaDepth})
                cmp(r);
        for (u32 o = 0; o < 8; o++)
            cmp(kViewportTransform + o);
        for (u32 o = 0; o < 4; o++)
            cmp(kViewport + o);
        cmp(0x380);
        if (d.regs[0x380] & 1)
            cmp(0x381), cmp(0x382);
        // vertex input: attributes, and the streams the fetched ones use
        for (u32 a = 0; a < 32; a++) {
            cmp(kVertexAttribFormat + a);
            const u32 fa = d.regs[kVertexAttribFormat + a];
            if (!(fa & 0x40) && (fa & 0x1F) < 16) {
                const u32 b = fa & 0x1F;
                for (u32 o = 0; o < 4; o++)
                    cmp(kVertexArray + b * 4 + o);
                cmp(kVertexArrayLimit + b * 2), cmp(kVertexArrayLimit + b * 2 + 1);
            }
        }
        // indirect draws: the encoder read their parameters before the GPU computed them
        const bool indirect = e.cmd >= 0 && v.cmd_names[e.cmd].find("Indirect") != std::string::npos;
        if (d.draw.indexed)
            for (u32 r : {kIndexAddrHigh, kIndexAddrLow, kIndexFormat, kIndexFirst, kIndexCount})
                if (!indirect || r < kIndexFirst)
                    cmp(r);
        static const u32 singles[] = {
            kDepthTestEnable, kDepthWriteEnable, kDepthTestFunc, kAlphaTestEnable, kAlphaTestRef, kAlphaTestFunc,
            kBlendColor, kBlendColor + 1, kBlendColor + 2, kBlendColor + 3, 0x4B9, kBlendSeparateAlpha, kBlendEqRgb,
            kBlendSrcRgb, kBlendDstRgb, kBlendEqA, kBlendSrcA, kBlendDstA, kStencilEnable, kStencilFrontOpFail,
            kStencilFrontOpZFail, kStencilFrontOpZPass, kStencilFrontFunc, kStencilFrontRef, kStencilFrontFuncMask,
            kStencilFrontMask, kStencilTwoSide, kStencilBackOpFail, kStencilBackOpZFail, kStencilBackOpZPass,
            kStencilBackFunc, 0x3D5, 0x3D6, 0x3D7, kScreenYControl, 0x50D, 0x50E, kCullEnable, kFrontFace, kCullFace,
            kPolygonOffsetFillEnable, kSlopeScaleDepthBias, kDepthBias, kDepthBiasClamp, kTscAddrHigh,
            kTscAddrLow, kTicAddrHigh, kTicAddrLow, kTexCbIndex, 0x35F, 0x64F, kViewportTransformEnable, 0x591, 0x592};
        const bool depth_test = d.regs[kDepthTestEnable] & 1, stencil = d.regs[kStencilEnable] & 1;
        for (u32 r : singles) {
            if (!depth_test && (r == kDepthWriteEnable || r == kDepthTestFunc))
                continue;
            if (!stencil && ((r >= kStencilFrontOpFail && r <= kStencilFrontMask) || (r >= kStencilBackOpFail && r <= kStencilBackFunc) ||
                             (r >= 0x3D5 && r <= 0x3D7) || r == kStencilTwoSide))
                continue;
            cmp(r);
        }
        for (u32 k = 0; k < 8; k++) {
            cmp(kBlendEnable + k);
            cmp(kColorMask + k);
            if (d.regs[0x4B9] & 1)
                for (u32 o = 0; o < 7; o++)
                    cmp(kIndependentBlend + k * 8 + o);
        }
        // programs, draw parameters
        for (u32 p = 0; p < 6; p++)
            if (d.prog[p] != e.prog[p])
                add(fmt::format("program {}", p), i, d.prog[p], e.prog[p]);
        if (d.draw.topology != e.draw.topology)
            add("draw topology", i, d.draw.topology, e.draw.topology);
        if (d.draw.indexed != e.draw.indexed && !indirect)
            add("draw indexed", i, d.draw.indexed, e.draw.indexed);
        if (d.draw.first != e.draw.first && !indirect)
            add("draw first", i, d.draw.first, e.draw.first);
        if (d.draw.count != e.draw.count && !indirect)
            add("draw count", i, d.draw.count, e.draw.count);
        // constant buffer bindings and the driver-owned buffers' contents
        for (u32 st = 0; st < 5; st++)
            for (u32 c = 0; c < 18; c++) {
                const auto &a = d.cb[st][c], &b = e.cb[st][c];
                if (!a.enabled && !b.enabled)
                    continue;
                if (a.enabled != b.enabled)
                    add(fmt::format("s{}c{} enabled", st, c), i, a.enabled, b.enabled);
                else if (c >= 3 && (a.address != b.address || a.size != b.size))
                    add(fmt::format("s{}c{} binding", st, c), i, a.address | (u64(a.size) << 44), b.address | (u64(b.size) << 44));
                if (c < 3 && a.enabled && b.enabled) {
                    const auto &x = d.cbdata[st][c], &y = e.cbdata[st][c];
                    for (size_t w = 0; w < std::max(x.size(), y.size()); w++) {
                        const u32 xv = w < x.size() ? x[w] : 0, yv = w < y.size() ? y[w] : 0;
                        if (xv != yv)
                            add(fmt::format("s{}c{}[{:#05x}]", st, c, w * 4), i, xv, yv);
                    }
                }
            }
    }
    f << fmt::format("# matched {} of {} encoder draws, {} register comparisons\n", matched, E.size(), compared);
    f << fmt::format("# shadowless binds {}\n", v.no_shadow);
    for (const auto& [name, n] : v.unhandled)
        f << fmt::format("# unhandled {} x{}\n", name, n);
    std::vector<std::pair<std::string, const Diff*>> sorted;
    for (const auto& [k, d] : diffs)
        sorted.emplace_back(k, &d);
    std::stable_sort(sorted.begin(), sorted.end(), [](auto& a, auto& b) { return a.second->count > b.second->count; });
    for (const auto& [k, d] : sorted) {
        f << fmt::format("{:<28} {:>6}  ", k, d->count);
        for (const auto& ex : d->examples)
            f << ex << "; ";
        f << "\n";
    }
    ReportCompute(f);
    ReportCopies(f);
    LOG_INFO("NVN", "verify: {} of {} encoder draws matched, {} differing items; wrote nvn_verify.txt", matched, E.size(),
             diffs.size());
}

} // namespace

// MUA3_NVN_REPLAY: every draw the command stream reaches is matched (by its programs) with the encoder's next draw
// command, which is then rendered from the encoder's register file instead; draws the encoder does not have (driver
// internal) render as before. Copies, compute and sync still come from the stream.
struct Replay {
    Gpu* gpu = nullptr;
    std::unique_ptr<Encoder> enc;
    u32 skip = 0; // remaining instances of the last matched draw
    u64 skip_vs = 0, skip_fs = 0;
    u32 misses_in_row = 0;
    u64 matched = 0, own = 0, lost = 0, frames_logged = 0, cs_matched = 0, cs_own = 0;
};

void InstallReplay(Gpu& gpu) {
    static Replay r;
    r.gpu = &gpu;
    r.enc = std::make_unique<Encoder>(gpu);
    gpu.StreamM3D().suppress_clears = true;
    gpu.StreamM3D().intercept = [](const Maxwell3D& m) -> bool {
        const u64 base = m.Addr(kCodeAddrHigh);
        const u64 vs = base + m.Reg(kShaderConfig + 0x10 + 1);
        const u64 fs = (m.Reg(kShaderConfig + 0x50) & 1) ? base + m.Reg(kShaderConfig + 0x50 + 1) : 0;
        if (r.skip && vs == r.skip_vs && fs == r.skip_fs) {
            r.skip--;
            return true;
        }
        r.skip = 0;
        Encoder& e = *r.enc;
        struct Active { // the renderer reads the encoder's registers while it encodes (clears, draws)
            Gpu& g;
            explicit Active(Gpu& gg, Maxwell3D& mm) : g(gg) { g.SetActiveM3D(&mm); }
            ~Active() { g.SetActiveM3D(nullptr); }
        } active(*r.gpu, e.M());
        for (int tries = 0; tries < 64 && e.ToDraw(); tries++) {
            if (e.PendingDispatch() || e.DrawVs() != vs || e.DrawFs() != fs) {
                // an encoder draw without instances has no stream counterpart: pass it; otherwise this is the
                // stream's own draw, and the encoder waits
                if (!e.PendingIssuesNothing())
                    break;
                e.RunDraw();
                continue;
            }
            const u32 n = e.RunDraw();
            r.misses_in_row = 0;
            r.matched++;
            r.skip = n ? n - 1 : 0, r.skip_vs = vs, r.skip_fs = fs;
            if (++r.frames_logged % 20000 == 0)
                LOG_INFO("NVN", "replay: {} draws from the encoder, {} from the stream, {} resyncs, {} dropped submits; "
                                "{} dispatches from the encoder, {} from the stream",
                         r.matched, r.own, r.lost, e.dropped, r.cs_matched, r.cs_own);
            return n > 0;
        }
        if (++r.misses_in_row >= 64) {
            r.lost++, r.misses_in_row = 0;
            e.Discard();
            LOG_WARN("NVN", "replay: lost step with the command stream (draw vs {:#x} fs {:#x}); skipped the rest of the submit", vs, fs);
        }
        r.own++;
        return false;
    };
    if (!std::getenv("MUA3_NVN_REPLAY_NO_COMPUTE")) {
        r.enc->render_compute = true;
        gpu.intercept_dispatch = [](const u32* regs, const u32* q) -> bool {
            const u64 prog = ((u64(regs[0x582] & 0xFF) << 32) | regs[0x583]) + q[8];
            const u32 grid[3] = {q[0xC] & 0x7FFFFFFF, q[0xD] & 0xFFFF, q[0xD] >> 16};
            Encoder& e = *r.enc;
            struct Active {
                Gpu& g;
                explicit Active(Gpu& gg, Maxwell3D& mm) : g(gg) { g.SetActiveM3D(&mm); }
                ~Active() { g.SetActiveM3D(nullptr); }
            } active(*r.gpu, e.M());
            for (int tries = 0; tries < 64 && e.ToDraw(); tries++) {
                const bool same = e.PendingDispatch() && e.DispatchProgram() == prog && e.DispatchGrid(0) == grid[0] &&
                                  e.DispatchGrid(1) == grid[1] && e.DispatchGrid(2) == grid[2];
                if (!same) {
                    if (!e.PendingIssuesNothing())
                        break;
                    e.RunDraw();
                    continue;
                }
                e.RunDraw();
                r.cs_matched++;
                return true;
            }
            r.cs_own++;
            return false;
        };
    }
    hle::nvn::SetReplaySink([](std::vector<hle::nvn::ReplaySpan>&& spans) { r.enc->Queue(Frame(std::move(spans))); });
    LOG_INFO("NVN", "native replay: draws come from the host NVN encoder");
}

// MUA3_NVN_NATIVE: the driver never sees the command buffer commands the encoder handles (hle::nvn::Native());
// each submit's commands are encoded and rendered on the GPU thread, in order with the driver's own submissions
// (copies, fences, present).
void InstallNative(Gpu& gpu) {
    static Gpu* g = &gpu;
    static Encoder* enc = new Encoder(gpu);
    enc->render_compute = true;
    hle::nvn::SetReplaySink([](std::vector<hle::nvn::ReplaySpan>&& spans) {
        auto list = std::make_shared<Frame>(std::move(spans));
        g->SubmitHost([list] {
            static u64 jobs = 0, ns = 0, cmds = 0;
            const u64 t0 = g->NowNs();
            g->SetActiveM3D(&enc->M());
            enc->Run(*list);
            g->SetActiveM3D(nullptr);
            ns += g->NowNs() - t0, cmds += list->size();
            if (++jobs % 600 == 0) {
                LOG_INFO("NVN", "native: {:.0f} commands per submit, {:.2f} ms each on the GPU thread", cmds / 600.0, ns / 600e6);
                ns = cmds = 0;
            }
        });
    });
    LOG_INFO("NVN", "native: command buffers are encoded and rendered by the host");
}

void InstallNvnEncoder(Gpu& gpu) {
    if (hle::nvn::Native()) {
        InstallNative(gpu);
        return;
    }
    if (std::getenv("MUA3_NVN_REPLAY")) {
        InstallReplay(gpu);
        return;
    }
    const char* e = std::getenv("MUA3_NVN_VERIFY");
    if (!e)
        return;
    auto& v = V();
    v.gpu = &gpu;
    v.at_ms = std::strtoull(e, nullptr, 10) * 1000;
    v.enc = std::make_unique<Encoder>(gpu);
    // The driver path's draws of the tagged submit.
    gpu.StreamM3D().intercept = [&v](const Maxwell3D& m) {
        if (v.phase.load() != 2)
            return false;
        v.drv_snaps.push_back(TakeSnap(*v.gpu, m, -1));
        if (!v.copies_mode && v.drv_snaps.size() >= v.want) {
            v.phase = 3;
            Report();
        }
        return false;
    };
    gpu.intercept_dispatch = [&v](const u32* r, const u32* q) {
        if (v.phase.load() == 2)
            v.drv_cs.push_back(TakeCSnap(*v.gpu, r, q, -1));
        return false;
    };
    gpu.intercept_dma = [&v](const u32* r, u32 launch) {
        if (v.phase.load() == 2) {
            v.drv_copy.push_back(Canonical(r, launch));
            if (v.copies_mode && (v.drv_copy.size() >= v.want ||
                                  std::chrono::steady_clock::now() - v.collect_start > std::chrono::seconds(5))) {
                v.phase = 3;
                Report();
            }
        }
        return false;
    };
    gpu.on_tagged_job = [&v] {
        if (v.phase.load() == 1) {
            v.collect_start = std::chrono::steady_clock::now();
            v.phase = 2;
        }
    };
    hle::nvn::SetReplaySink([&v](std::vector<hle::nvn::ReplaySpan>&& spans) {
        const Frame cmds(std::move(spans));
        size_t draws = 0, copies = 0;
        for (const auto& c : cmds) {
            draws += c.name.find("CommandBufferDraw") != std::string_view::npos;
            copies += c.name.find("CommandBufferCopy") != std::string_view::npos;
        }
        const bool want_copies = v.copies_mode;
        const bool capture = v.phase.load() == 0 && (want_copies ? copies >= 4 : draws >= 100) &&
                             u64(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - v.t0).count()) >= v.at_ms;
        Maxwell3D& m = v.enc->M();
        if (capture) {
            m.capture = [&v](const Maxwell3D& mm) { v.enc_snaps.push_back(TakeSnap(*v.gpu, mm, v.enc->current)); };
            v.enc->on_copy = [&v](const DmaLaunch& l) {
                v.enc_copy.push_back(Canonical(l.r.data(), l.launch));
                v.enc_copy_cmd.push_back(v.enc->current);
            };
            v.enc->on_dispatch = [&v](const Encoder::Launch& l) {
                auto c = TakeCSnap(*v.gpu, l.r.data(), l.q.data(), v.enc->current);
                c.code = l.code, c.control = l.control;
                c.control_words.resize(0x900 / 4);
                if (l.control && v.gpu->Mem().IsValid(l.control, 0x900))
                    v.gpu->Mem().ReadBlock(l.control, c.control_words.data(), 0x900);
                v.enc_cs.push_back(std::move(c));
            };
            for (const auto& c : cmds)
            {
                v.cmd_names.emplace_back(c.name.substr(16));
                std::string l = fmt::format("{} {} {:x} {:x} {:x} {:x} {:x} {:x} |", v.cmd_lines.size(), c.name.substr(16), c.x[1],
                                            c.x[2], c.x[3], c.x[4], c.x[5], c.x[6]);
                for (u32 f : c.v)
                    l += fmt::format(" {}", U2F(f));
                for (u64 e : c.extra)
                    l += fmt::format(" e{:x}", e);
                if (!c.raw.empty())
                    l += " raw";
                for (u64 r : c.raw)
                    l += fmt::format(" {:x}", r);
                for (const auto& o : c.objects) {
                    l += " {";
                    for (const StateCall& k : o)
                        l += fmt::format(" {}({:x},{:x},{:x},{:x})", k.name.substr(k.name.find("Set") + 3), k.x[1], k.x[2],
                                         k.x[3], k.x[4]);
                    l += " }";
                }
                if (c.name.ends_with("SetRenderTargets")) {
                    std::vector<u64> texs = c.extra;
                    texs.push_back(c.x[4]);
                    for (u64 t : texs) {
                        hle::nvn::TextureInfo ti;
                        if (hle::nvn::QueryTexture(t, ti))
                            l += fmt::format(" [{:x}: {:#x} {}x{} fmt {:#x}]", t, ti.gpu, ti.width, ti.height, ti.format);
                        else
                            l += fmt::format(" [{:x}: unknown]", t);
                    }
                }
                v.cmd_lines.push_back(std::move(l));
            }
        } else {
            m.capture = [](const Maxwell3D&) {};
            v.enc->on_dispatch = nullptr;
            v.enc->on_copy = [](const DmaLaunch&) {};
        }
        v.enc->Run(cmds);
        if (capture) {
            v.unhandled = v.enc->unhandled;
            v.no_shadow = v.enc->no_shadow;
            v.want = want_copies ? v.enc_copy.size() + 64 : v.enc_snaps.size() + v.enc_snaps.size() / 4 + 32;
            LOG_INFO("NVN", "verify: encoded {} commands, {} draws; waiting for the driver's", cmds.size(), v.enc_snaps.size());
            v.phase = 1;
            v.gpu->tag_next_submit = true;
        }
    });
    LOG_INFO("NVN", "encoder verification armed at {} s", v.at_ms / 1000);
}

} // namespace gpu
