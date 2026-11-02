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
std::atomic<u64> g_indirect_known{0}, g_indirect_unknown{0}, g_indirect_arrays{0};


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
                 n == "SetShaderScratchMemory" || n == "SetRenderEnable" ||
                 n == "SetRenderEnableConditional") {
            // no renderer state (render enables: the emulated path draws unconditionally too)
        } else if (n == "ReportCounter") { // (cb, counter type, address) -> {value, GPU timestamp}
            if (render_compute) {
                const u64 v[2] = {0, g_.GpuTimestamp()};
                g_.WriteGpu(x[2], v, sizeof(v));
            }
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
            // The culling list draws: the host wrote the list and knows the count, so the arguments the GPU
            // produced from its counter are not read back (that read waits for the whole frame so far).
            u32 known = 0;
            if (render_compute && g_.GetRenderer())
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
            M().draw.indirect_args = x[2];
            Draw(u32(x[1]), false, 0, 1, 0, 0, 0, 1, 0);
            M().draw.indirect_args = 0;
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
            if (c.extra.size() >= 2) // the program's first stage {code, control}, captured at bind
                code = c.extra[0], control = c.extra[1];
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

    // NVNvertexAttribState (4 bytes): stream index bits 0-4, "set" bit 6, offset bits 7-20, NVN format from bit 21.
    void BindAttribs(const ReplayCommand& c) {
        const u32 count = std::min<u32>(u32(c.x[1]), 32);
        ResetAttribs();
        for (u32 i = 0; i < count; i++) {
            const u32 w = Raw32(c, i);
            const u32 fmt = w >> 21, offset = (w >> 7) & 0x3FFF, stream = w & 0x1F;
            const u32 hw = AttribFormat(fmt);
            if (!hw) {
                if (fmt)
                    unhandled[fmt::format("vertex format {:#x}", fmt)]++;
                continue;
            }
            attribs_[i] = stream | (offset << 7) | hw;
        }
    }

    // NVNvertexStreamState (8 bytes): stride, divisor.
    void BindStreams(const ReplayCommand& c) {
        const u32 count = std::min<u32>(u32(c.x[1]), 16);
        for (u32 i = 0; i < count; i++)
            streams_[i].stride = Raw32(c, i * 2), streams_[i].divisor = Raw32(c, i * 2 + 1);
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

    // State objects are decoded from their raw words (captured at bind; the game builds many on the stack).
    // NVNblendState: byte 0 target, byte 3 equations (rgb | alpha << 4), bytes 4-7 src/dst rgb, src/dst alpha.
    void ApplyBlend(const ReplayCommand& c) {
        if (c.raw.empty())
            return;
        const u64 w = c.raw[0];
        const u32 target = u32(w) & 7, eq = u32(w >> 24) & 0xF, eq_a = u32(w >> 28) & 0xF;
        const u32 src = u32(w >> 32) & 0xFF, dst = u32(w >> 40) & 0xFF, src_a = u32(w >> 48) & 0xFF, dst_a = u32(w >> 56) & 0xFF;
        const u32 b = kIndependentBlend + target * 8;
        W(b + 0, 0);
        W(b + 1, eq), W(b + 2, src), W(b + 3, dst), W(b + 4, eq_a), W(b + 5, src_a), W(b + 6, dst_a);
        W(0x4B9, 1);
    }

    // NVNcolorState: byte 1 blend enable per target, byte 3 alpha test function (8 = always).
    void ApplyColor(const ReplayCommand& c) {
        if (c.raw.empty())
            return;
        const u32 w = u32(c.raw[0]);
        const u32 enable = (w >> 8) & 0xFF, alpha_func = (w >> 24) & 0xF;
        for (u32 i = 0; i < 8; i++)
            W(kBlendEnable + i, (enable >> i) & 1);
        W(kAlphaTestEnable, alpha_func && alpha_func != 8 ? 1 : 0);
        W(kAlphaTestFunc, alpha_func && alpha_func != 8 ? alpha_func : 0);
    }

    // NVNchannelMaskState: 4 bits (r, g, b, a) per target.
    void ApplyChannelMask(const ReplayCommand& c) {
        if (c.raw.empty())
            return;
        const u32 w = u32(c.raw[0]);
        for (u32 i = 0; i < 8; i++) {
            const u32 m = (w >> (i * 4)) & 0xF;
            W(kColorMask + i, (m & 1) | ((m & 2) << 3) | ((m & 4) << 6) | ((m & 8) << 9));
        }
    }

    // NVNdepthStencilState: bit 0 depth test, 1 depth write, 2 stencil test, bits 4-7 depth function; from bit 32 one
    // 16-bit word per face (front, back): function, fail, pass, depth fail ops (4 bits each).
    void ApplyDepthStencil(const ReplayCommand& c) {
        if (c.raw.empty())
            return;
        const u64 w = c.raw[0];
        W(kDepthTestEnable, u32(w) & 1);
        W(kDepthWriteEnable, (u32(w) >> 1) & 1);
        W(kDepthTestFunc, (u32(w) >> 4) & 0xF);
        W(kStencilEnable, (u32(w) >> 2) & 1);
        const u32 front = u32(w >> 32) & 0xFFFF, back = u32(w >> 48) & 0xFFFF;
        W(kStencilFrontFunc, front & 0xF);
        W(kStencilFrontOpFail, (front >> 4) & 0xF), W(kStencilFrontOpZPass, (front >> 8) & 0xF), W(kStencilFrontOpZFail, front >> 12);
        W(kStencilTwoSide, 1);
        W(kStencilBackFunc, back & 0xF);
        W(kStencilBackOpFail, (back >> 4) & 0xF), W(kStencilBackOpZPass, (back >> 8) & 0xF), W(kStencilBackOpZFail, back >> 12);
    }

    // NVNpolygonState: bits 0-1 cull face, bit 2 front face counter-clockwise, bits 3-4 polygon mode, bits 15-17
    // polygon offset enables (point, line, fill).
    void ApplyPolygon(const ReplayCommand& c) {
        if (c.raw.empty())
            return;
        const u32 w = u32(c.raw[0]);
        const u32 cull = w & 3;
        W(kCullEnable, cull ? 1 : 0);
        W(kCullFace, cull == 1 ? 0x404 : cull == 3 ? 0x408 : 0x405);
        W(kFrontFace, (w >> 2) & 1 ? 0x901 : 0x900);
        W(kPolygonOffsetFillEnable, (w >> 17) & 1);
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

} // namespace

// The driver never sees the command buffer commands the encoder handles: each submit's commands are encoded and
// rendered on the GPU thread, in submission order with the host's fences and presents.
void InstallNative(Gpu& gpu) {
    static Gpu* g = &gpu;
    static Encoder* enc = new Encoder(gpu);
    enc->render_compute = true;
    hle::nvn::SetReplaySink([](std::vector<hle::nvn::ReplaySpan>&& spans) {
        auto list = std::make_shared<Frame>(std::move(spans));
        g->SubmitHost([list] {
            static u64 jobs = 0, ns = 0, cmds = 0;
            const u64 t0 = g->NowNs();
            hle::nvn::TimelineEvent("job-start", list->size());
            g->SetActiveM3D(&enc->M());
            enc->Run(*list);
            g->SetActiveM3D(nullptr);
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

void InstallNvnEncoder(Gpu& gpu) { InstallNative(gpu); }

} // namespace gpu
