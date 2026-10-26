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
#include "gpu/gpu.h"
#include "gpu/maxwell3d.h"
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

    void Run(const std::vector<ReplayCommand>& cmds) {
        for (size_t i = 0; i < cmds.size(); i++) {
            current = int(i);
            Encode(cmds[i]);
        }
        current = -1;
    }

    // ---- native replay in step with the command stream (MUA3_NVN_REPLAY): submits are queued by the game thread and
    // encoded on the GPU thread, one draw command at a time.
    void Queue(std::vector<ReplayCommand>&& cmds) {
        bool draws = false;
        for (const auto& c : cmds)
            draws |= c.name.find("CommandBufferDraw") != std::string_view::npos;
        std::lock_guard lk(queue_mutex_);
        // submits without draws (uploads while loading) join the previous one: only draws drain the queue
        if (!draws && !queue_.empty()) {
            auto& last = queue_.back();
            last.insert(last.end(), std::make_move_iterator(cmds.begin()), std::make_move_iterator(cmds.end()));
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
            if (c.name.find("CommandBufferDraw") != std::string_view::npos)
                return true;
            Encode(c);
            pos_++;
        }
    }
    // Programs of the pending draw (vertex, fragment Maxwell program header addresses).
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
        u32 a[2]{};
        if (c.name.ends_with("DrawElementsIndirect"))
            g_.ReadGpu(c.x[4], a, sizeof(a));
        else if (c.name.ends_with("DrawArraysIndirect"))
            g_.ReadGpu(c.x[2], a, sizeof(a));
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
        } else if (n == "BindStorageBuffer") {
            // the driver's table in the shared c0: {address lo, hi, size, 0} at 0x110 + hardware stage * 0x100
            if (x[1] < 5 && x[2] < 16) {
                const u32 off = 0x110 + kHwStage[x[1]] * 0x100 + u32(x[2]) * 16;
                DrvWrite(0, 0, off, u32(x[3]));
                DrvWrite(0, 0, off + 4, u32(x[3] >> 32));
                DrvWrite(0, 0, off + 8, u32(x[4]));
            }
        } else if (n == "BindTexture") {
            if (x[1] < 5 && x[2] < 128)
                DrvWrite(kHwStage[x[1]], 2, 0x20 + u32(x[2]) * 8, u32(x[3]));
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
        } else if (n == "DrawElementsBaseVertex")
            Draw(u32(x[1]), true, 0, u32(x[3]), u32(x[2]), x[4], s32(x[5]), 1, 0);
        else if (n == "DrawElements")
            Draw(u32(x[1]), true, 0, u32(x[3]), u32(x[2]), x[4], 0, 1, 0);
        else if (n == "DrawArrays")
            Draw(u32(x[1]), false, u32(x[2]), u32(x[3]), 0, 0, 0, 1, 0);
        else if (n == "DrawElementsIndirect") {
            u32 a[5]{}; // count, instances, first index, base vertex, base instance
            g_.ReadGpu(x[4], a, sizeof(a));
            Draw(u32(x[1]), true, a[2], a[0], u32(x[2]), x[3], s32(a[3]), a[1], a[4]);
        } else if (n == "DrawArraysIndirect") {
            u32 a[4]{}; // count, instances, first, base instance
            g_.ReadGpu(x[2], a, sizeof(a));
            Draw(u32(x[1]), false, a[2], a[0], 0, 0, 0, a[1], a[3]);
        } else
            unhandled[std::string(n)]++;
    }

    u32 Raw32(const ReplayCommand& c, u32 i) const {
        return i / 2 < c.raw.size() ? u32(c.raw[i / 2] >> ((i & 1) * 32)) : 0;
    }

    void BindProgram(const ReplayCommand& c) {
        u64 code = 0;
        if (c.x[1]) {
            if (!c.objects.empty())
                for (const StateCall& k : c.objects[0])
                    if (Is(k.name, "SetShaders") && !k.handles.empty())
                        code = k.handles[0];
            if (!code)
                code = hle::nvn::QueryProgramCode(c.x[1]), no_shadow++;
        }
        for (u32 s = 0; s < 6; s++)
            if (c.x[2] & (1u << s))
                prog_[s] = code ? code + 0x30 : 0; // the Maxwell program header follows NVN's 0x30-byte blob header
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
    std::mutex queue_mutex_;
    std::deque<std::vector<ReplayCommand>> queue_;
    std::vector<ReplayCommand> frame_;
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
    u64 matched = 0, own = 0, lost = 0, frames_logged = 0;
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
            if (e.DrawVs() != vs || e.DrawFs() != fs) {
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
                LOG_INFO("NVN", "replay: {} draws from the encoder, {} from the stream, {} resyncs, {} dropped submits",
                         r.matched, r.own, r.lost, e.dropped);
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
    hle::nvn::SetReplaySink([](std::vector<ReplayCommand>&& cmds) { r.enc->Queue(std::move(cmds)); });
    LOG_INFO("NVN", "native replay: draws come from the host NVN encoder");
}

void InstallNvnEncoder(Gpu& gpu) {
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
        if (v.drv_snaps.size() >= v.want) {
            v.phase = 3;
            Report();
        }
        return false;
    };
    gpu.on_tagged_job = [&v] {
        if (v.phase.load() == 1)
            v.phase = 2;
    };
    hle::nvn::SetReplaySink([&v](std::vector<ReplayCommand>&& cmds) {
        size_t draws = 0;
        for (const auto& c : cmds)
            draws += c.name.find("CommandBufferDraw") != std::string_view::npos;
        const bool capture = v.phase.load() == 0 && draws >= 100 &&
                             u64(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - v.t0).count()) >= v.at_ms;
        Maxwell3D& m = v.enc->M();
        if (capture) {
            m.capture = [&v](const Maxwell3D& mm) { v.enc_snaps.push_back(TakeSnap(*v.gpu, mm, v.enc->current)); };
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
        }
        v.enc->Run(cmds);
        if (capture) {
            v.unhandled = v.enc->unhandled;
            v.no_shadow = v.enc->no_shadow;
            v.want = v.enc_snaps.size() + v.enc_snaps.size() / 4 + 32;
            LOG_INFO("NVN", "verify: encoded {} commands, {} draws; waiting for the driver's", cmds.size(), v.enc_snaps.size());
            v.phase = 1;
            v.gpu->tag_next_submit = true;
        }
    });
    LOG_INFO("NVN", "encoder verification armed at {} s", v.at_ms / 1000);
}

} // namespace gpu
