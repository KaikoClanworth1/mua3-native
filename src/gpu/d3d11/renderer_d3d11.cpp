#include "gpu/d3d11/renderer_d3d11.h"

#include <algorithm>

#include <d3d11sdklayers.h>
#include <d3dcompiler.h>

#include "core/memory.h"
#include "gpu/d3d11/shader_cache.h"
#include "gpu/d3d11/texture_cache.h"
#include "gpu/formats.h"
#include "gpu/gpu.h"

namespace gpu {

using namespace reg3d;

namespace {

DXGI_FORMAT Typeless(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: case DXGI_FORMAT_R8G8B8A8_SNORM:
    case DXGI_FORMAT_R8G8B8A8_UINT: case DXGI_FORMAT_R8G8B8A8_SINT: return DXGI_FORMAT_R8G8B8A8_TYPELESS;
    case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_TYPELESS;
    case DXGI_FORMAT_B8G8R8X8_UNORM: case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8X8_TYPELESS;
    case DXGI_FORMAT_D24_UNORM_S8_UINT: return DXGI_FORMAT_R24G8_TYPELESS;
    case DXGI_FORMAT_D32_FLOAT: return DXGI_FORMAT_R32_TYPELESS;
    case DXGI_FORMAT_D16_UNORM: return DXGI_FORMAT_R16_TYPELESS;
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return DXGI_FORMAT_R32G8X24_TYPELESS;
    default: return f;
    }
}

DXGI_FORMAT DepthSrvFormat(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_D24_UNORM_S8_UINT: return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case DXGI_FORMAT_D32_FLOAT: return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_D16_UNORM: return DXGI_FORMAT_R16_UNORM;
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    default: return f;
    }
}

D3D11_BLEND BlendFactor(u32 f) {
    if (f & 0x4000) { // GL encoding
        switch (f & 0xBFFF) {
        case 0x0000: return D3D11_BLEND_ZERO;
        case 0x0001: return D3D11_BLEND_ONE;
        case 0x0300: return D3D11_BLEND_SRC_COLOR;
        case 0x0301: return D3D11_BLEND_INV_SRC_COLOR;
        case 0x0302: return D3D11_BLEND_SRC_ALPHA;
        case 0x0303: return D3D11_BLEND_INV_SRC_ALPHA;
        case 0x0304: return D3D11_BLEND_DEST_ALPHA;
        case 0x0305: return D3D11_BLEND_INV_DEST_ALPHA;
        case 0x0306: return D3D11_BLEND_DEST_COLOR;
        case 0x0307: return D3D11_BLEND_INV_DEST_COLOR;
        case 0x0308: return D3D11_BLEND_SRC_ALPHA_SAT;
        case 0x8001: case 0x8003: return D3D11_BLEND_BLEND_FACTOR;
        case 0x8002: case 0x8004: return D3D11_BLEND_INV_BLEND_FACTOR;
        case 0x88F9: return D3D11_BLEND_SRC1_COLOR;
        case 0x88FA: return D3D11_BLEND_INV_SRC1_COLOR;
        case 0x8589: return D3D11_BLEND_SRC1_ALPHA;
        case 0x88FB: return D3D11_BLEND_INV_SRC1_ALPHA;
        }
    }
    switch (f) {
    case 0x01: return D3D11_BLEND_ZERO;
    case 0x02: return D3D11_BLEND_ONE;
    case 0x03: return D3D11_BLEND_SRC_COLOR;
    case 0x04: return D3D11_BLEND_INV_SRC_COLOR;
    case 0x05: return D3D11_BLEND_SRC_ALPHA;
    case 0x06: return D3D11_BLEND_INV_SRC_ALPHA;
    case 0x07: return D3D11_BLEND_DEST_ALPHA;
    case 0x08: return D3D11_BLEND_INV_DEST_ALPHA;
    case 0x09: return D3D11_BLEND_DEST_COLOR;
    case 0x0A: return D3D11_BLEND_INV_DEST_COLOR;
    case 0x0B: return D3D11_BLEND_SRC_ALPHA_SAT;
    case 0x0E: case 0x0F: return D3D11_BLEND_BLEND_FACTOR;
    case 0x10: return D3D11_BLEND_SRC1_COLOR;
    case 0x11: return D3D11_BLEND_INV_SRC1_COLOR;
    case 0x12: return D3D11_BLEND_SRC1_ALPHA;
    case 0x13: return D3D11_BLEND_INV_SRC1_ALPHA;
    default: return D3D11_BLEND_ONE;
    }
}

D3D11_BLEND AlphaFactor(D3D11_BLEND b) {
    switch (b) {
    case D3D11_BLEND_SRC_COLOR: return D3D11_BLEND_SRC_ALPHA;
    case D3D11_BLEND_INV_SRC_COLOR: return D3D11_BLEND_INV_SRC_ALPHA;
    case D3D11_BLEND_DEST_COLOR: return D3D11_BLEND_DEST_ALPHA;
    case D3D11_BLEND_INV_DEST_COLOR: return D3D11_BLEND_INV_DEST_ALPHA;
    case D3D11_BLEND_SRC1_COLOR: return D3D11_BLEND_SRC1_ALPHA;
    case D3D11_BLEND_INV_SRC1_COLOR: return D3D11_BLEND_INV_SRC1_ALPHA;
    default: return b;
    }
}

D3D11_BLEND_OP BlendOp(u32 e) {
    switch (e) {
    case 0x8006: case 1: return D3D11_BLEND_OP_ADD;
    case 0x800A: case 2: return D3D11_BLEND_OP_SUBTRACT;
    case 0x800B: case 3: return D3D11_BLEND_OP_REV_SUBTRACT;
    case 0x8007: case 4: return D3D11_BLEND_OP_MIN;
    case 0x8008: case 5: return D3D11_BLEND_OP_MAX;
    default: return D3D11_BLEND_OP_ADD;
    }
}

D3D11_COMPARISON_FUNC CompareFunc(u32 f) {
    if (f >= 0x200 && f <= 0x207)
        f -= 0x200;
    else if (f >= 1 && f <= 8)
        f -= 1;
    static const D3D11_COMPARISON_FUNC t[] = {D3D11_COMPARISON_NEVER, D3D11_COMPARISON_LESS,
                                              D3D11_COMPARISON_EQUAL, D3D11_COMPARISON_LESS_EQUAL,
                                              D3D11_COMPARISON_GREATER, D3D11_COMPARISON_NOT_EQUAL,
                                              D3D11_COMPARISON_GREATER_EQUAL, D3D11_COMPARISON_ALWAYS};
    return t[f & 7];
}

D3D11_STENCIL_OP StencilOp(u32 o) {
    switch (o) {
    case 0x1E00: case 1: return D3D11_STENCIL_OP_KEEP;
    case 0x0000: case 2: return D3D11_STENCIL_OP_ZERO;
    case 0x1E01: case 3: return D3D11_STENCIL_OP_REPLACE;
    case 0x1E02: case 4: return D3D11_STENCIL_OP_INCR_SAT;
    case 0x1E03: case 5: return D3D11_STENCIL_OP_DECR_SAT;
    case 0x150A: case 6: return D3D11_STENCIL_OP_INVERT;
    case 0x8507: case 7: return D3D11_STENCIL_OP_INCR;
    case 0x8508: case 8: return D3D11_STENCIL_OP_DECR;
    default: return D3D11_STENCIL_OP_KEEP;
    }
}

// Maxwell vertex attribute format register -> DXGI (size bits 21-26, type bits 27-29, BGRA bit 31).
DXGI_FORMAT AttribFormat(u32 f) {
    const u32 size = (f >> 21) & 0x3F, type = (f >> 27) & 7;
    const bool bgra = f >> 31;
    // type: 1 snorm, 2 unorm, 3 sint, 4 uint, 5 uscaled, 6 sscaled, 7 float
    auto pick = [&](DXGI_FORMAT fl, DXGI_FORMAT un, DXGI_FORMAT sn, DXGI_FORMAT ui, DXGI_FORMAT si) {
        switch (type) {
        case 1: return sn;
        case 2: return un;
        case 3: case 6: return si;
        case 4: case 5: return ui;
        default: return fl;
        }
    };
    constexpr auto U = DXGI_FORMAT_UNKNOWN;
    switch (size) {
    case 0x01: return pick(DXGI_FORMAT_R32G32B32A32_FLOAT, U, U, DXGI_FORMAT_R32G32B32A32_UINT, DXGI_FORMAT_R32G32B32A32_SINT);
    case 0x02: return pick(DXGI_FORMAT_R32G32B32_FLOAT, U, U, DXGI_FORMAT_R32G32B32_UINT, DXGI_FORMAT_R32G32B32_SINT);
    case 0x04: return pick(DXGI_FORMAT_R32G32_FLOAT, U, U, DXGI_FORMAT_R32G32_UINT, DXGI_FORMAT_R32G32_SINT);
    case 0x12: return pick(DXGI_FORMAT_R32_FLOAT, U, U, DXGI_FORMAT_R32_UINT, DXGI_FORMAT_R32_SINT);
    case 0x03: case 0x05:
        return pick(DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_UNORM, DXGI_FORMAT_R16G16B16A16_SNORM,
                    DXGI_FORMAT_R16G16B16A16_UINT, DXGI_FORMAT_R16G16B16A16_SINT);
    case 0x0F: return pick(DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_R16G16_UNORM, DXGI_FORMAT_R16G16_SNORM,
                           DXGI_FORMAT_R16G16_UINT, DXGI_FORMAT_R16G16_SINT);
    case 0x1B: return pick(DXGI_FORMAT_R16_FLOAT, DXGI_FORMAT_R16_UNORM, DXGI_FORMAT_R16_SNORM, DXGI_FORMAT_R16_UINT,
                           DXGI_FORMAT_R16_SINT);
    case 0x0A: case 0x13:
        if (bgra && type == 2)
            return DXGI_FORMAT_B8G8R8A8_UNORM;
        return pick(U, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_SNORM, DXGI_FORMAT_R8G8B8A8_UINT,
                    DXGI_FORMAT_R8G8B8A8_SINT);
    case 0x18: return pick(U, DXGI_FORMAT_R8G8_UNORM, DXGI_FORMAT_R8G8_SNORM, DXGI_FORMAT_R8G8_UINT, DXGI_FORMAT_R8G8_SINT);
    case 0x1D: return pick(U, DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_R8_SNORM, DXGI_FORMAT_R8_UINT, DXGI_FORMAT_R8_SINT);
    case 0x30: return type == 4 ? DXGI_FORMAT_R10G10B10A2_UINT : DXGI_FORMAT_R10G10B10A2_UNORM;
    case 0x31: return DXGI_FORMAT_R11G11B10_FLOAT;
    default: return U;
    }
}

struct DriverConstants {
    float viewport[4];
    u32 attrib[32][4];
    u32 vbuf[16][4];
    u32 swizzle[32][4];
    u32 misc[4];
};

} // namespace

// Per-renderer pooled GPU buffers and state objects.
class DrawContext {
public:
    explicit DrawContext(ID3D11Device* dev) : dev_(dev) {}

    ID3D11Buffer* Constant(u32 slot_key, const void* data, u32 size) {
        size = std::max<u32>(16, common::AlignUp(size, 16));
        auto& b = cbufs_[slot_key];
        if (!b.buf || b.size < size) {
            b.buf.Reset();
            b.size = std::max<u32>(size, 4096);
            D3D11_BUFFER_DESC d{b.size, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0};
            dev_->CreateBuffer(&d, nullptr, &b.buf);
        }
        Write(b.buf.Get(), data, size);
        return b.buf.Get();
    }
    struct Raw {
        ComPtr<ID3D11Buffer> buf;
        ComPtr<ID3D11ShaderResourceView> srv;
        u32 size = 0;
    };
    ID3D11ShaderResourceView* RawBuffer(u32 slot, const void* data, u32 size) {
        size = std::max<u32>(16, common::AlignUp(size, 16));
        auto& b = raw_[slot];
        if (!b.buf || b.size < size) {
            b.buf.Reset();
            b.srv.Reset();
            b.size = std::max<u32>(u32(common::AlignUp(size, 1 << 16)), 1 << 16);
            D3D11_BUFFER_DESC d{b.size, D3D11_USAGE_DYNAMIC, D3D11_BIND_SHADER_RESOURCE, D3D11_CPU_ACCESS_WRITE,
                                D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS, 0};
            dev_->CreateBuffer(&d, nullptr, &b.buf);
            D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
            sv.Format = DXGI_FORMAT_R32_TYPELESS;
            sv.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
            sv.BufferEx = {0, b.size / 4, D3D11_BUFFEREX_SRV_FLAG_RAW};
            dev_->CreateShaderResourceView(b.buf.Get(), &sv, &b.srv);
        }
        Write(b.buf.Get(), data, size);
        return b.srv.Get();
    }
    ID3D11Buffer* Vertex(u32 slot, const void* data, u32 size) {
        auto& b = vbufs_[slot];
        if (!b.buf || b.size < size) {
            b.buf.Reset();
            b.size = std::max<u32>(u32(common::AlignUp(size, 1 << 16)), 1 << 16);
            D3D11_BUFFER_DESC d{b.size, D3D11_USAGE_DYNAMIC, D3D11_BIND_VERTEX_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0};
            dev_->CreateBuffer(&d, nullptr, &b.buf);
        }
        Write(b.buf.Get(), data, size);
        return b.buf.Get();
    }
    std::unordered_map<u64, ComPtr<ID3D11InputLayout>> layouts;
    ComPtr<ID3D11Buffer> default_attrib; // float4(0,0,0,1) for disabled / constant attributes
    ID3D11Buffer* Index(const void* data, u32 size) {
        if (!index_ || index_size_ < size) {
            index_.Reset();
            index_size_ = std::max<u32>(u32(common::AlignUp(size, 1 << 16)), 1 << 16);
            D3D11_BUFFER_DESC d{index_size_, D3D11_USAGE_DYNAMIC, D3D11_BIND_INDEX_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0};
            dev_->CreateBuffer(&d, nullptr, &index_);
        }
        Write(index_.Get(), data, size);
        return index_.Get();
    }
    void SetContext(ID3D11DeviceContext* c) { ctx_ = c; }

    template <typename Desc, typename Obj, typename Create>
    Obj* Cached(std::unordered_map<u64, ComPtr<Obj>>& map, const Desc& d, Create create) {
        const u64 k = Hash64(&d, sizeof(d));
        auto it = map.find(k);
        if (it != map.end())
            return it->second.Get();
        ComPtr<Obj> o;
        create(&d, &o);
        map[k] = o;
        return o.Get();
    }
    std::unordered_map<u64, ComPtr<ID3D11BlendState>> blend;
    std::unordered_map<u64, ComPtr<ID3D11DepthStencilState>> depth;
    std::unordered_map<u64, ComPtr<ID3D11RasterizerState>> raster;

private:
    void Write(ID3D11Buffer* b, const void* data, u32 size) {
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(ctx_->Map(b, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
            std::memcpy(m.pData, data, size);
            ctx_->Unmap(b, 0);
        }
    }
    struct Cb {
        ComPtr<ID3D11Buffer> buf;
        u32 size = 0;
    };
    ID3D11Device* dev_;
    ID3D11DeviceContext* ctx_ = nullptr;
    std::unordered_map<u32, Cb> cbufs_;
    std::unordered_map<u32, Raw> raw_;
    std::unordered_map<u32, Cb> vbufs_;
    ComPtr<ID3D11Buffer> index_;
    u32 index_size_ = 0;
};

static std::unique_ptr<DrawContext> g_dc;

std::recursive_mutex& ContextMutex() {
    static std::recursive_mutex m;
    return m;
}

D3D11Renderer::D3D11Renderer(ID3D11Device* dev, ID3D11DeviceContext* ctx) : dev_(dev), ctx_(ctx) {
    shaders_ = std::make_unique<ShaderCache>(dev);
    textures_ = std::make_unique<TextureCache>(dev, ctx, *this);
    g_dc = std::make_unique<DrawContext>(dev);
    g_dc->SetContext(ctx);
    if (const char* t = std::getenv("MUA3_TRACE_DRAWS"))
        trace_draws_ = std::atoi(t);
    if (const char* d = std::getenv("MUA3_DEBUG_PS")) {
        debug_mode_ = std::atoi(d);
        const char* src = "float4 main() : SV_Target0 { return float4(1, 0, 1, 1); }";
        ComPtr<ID3DBlob> code, err;
        if (SUCCEEDED(D3DCompile(src, std::strlen(src), nullptr, nullptr, nullptr, "main", "ps_5_0", 0, 0, &code, &err)))
            dev_->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &debug_ps_);
    }
}

D3D11Renderer::~D3D11Renderer() = default;

Surface* D3D11Renderer::FindSurfaceByGpu(u64 gpu_va) {
    auto it = surfaces_.find(gpu_va);
    return it == surfaces_.end() ? nullptr : it->second.get();
}

Surface* D3D11Renderer::FindSurfaceByCpu(u64 cpu_va) {
    Surface* best = nullptr;
    for (auto& [k, s] : surfaces_)
        if (s->cpu_va == cpu_va && !s->depth && (!best || s->last_use > best->last_use))
            best = s.get();
    return best;
}

Surface* D3D11Renderer::GetSurface(u64 gpu_va, u32 width, u32 height, u32 format, bool depth) {
    if (!gpu_va || !width || !height || width > 16384 || height > 16384)
        return nullptr;
    auto it = surfaces_.find(gpu_va);
    if (it != surfaces_.end()) {
        Surface* s = it->second.get();
        if (s->width == width && s->height == height && s->format == format && s->depth == depth) {
            s->last_use = frame_counter_;
            return s;
        }
        surfaces_.erase(it);
    }
    const RtFormatInfo fi = depth ? ZetaFormat(format) : RtFormat(format);
    if (fi.dxgi == DXGI_FORMAT_UNKNOWN) {
        LOG_WARN("Renderer", "Unknown {} format {:#x}", depth ? "depth" : "color", format);
        return nullptr;
    }
    auto s = std::make_unique<Surface>();
    s->gpu_va = gpu_va;
    s->cpu_va = G().As().Translate(gpu_va);
    s->width = width;
    s->height = height;
    s->format = format;
    s->depth = depth;
    s->dxgi = fi.dxgi;
    s->last_use = frame_counter_;
    D3D11_TEXTURE2D_DESC td{width, height, 1, 1, Typeless(fi.dxgi), {1, 0}, D3D11_USAGE_DEFAULT,
                            UINT(D3D11_BIND_SHADER_RESOURCE | (depth ? D3D11_BIND_DEPTH_STENCIL : D3D11_BIND_RENDER_TARGET)),
                            0, 0};
    if (FAILED(dev_->CreateTexture2D(&td, nullptr, &s->tex))) {
        LOG_ERROR("Renderer", "Surface creation failed {}x{} fmt {:#x}", width, height, format);
        return nullptr;
    }
    D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sv.Texture2D.MipLevels = 1;
    if (depth) {
        D3D11_DEPTH_STENCIL_VIEW_DESC dv{};
        dv.Format = fi.dxgi;
        dv.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
        dev_->CreateDepthStencilView(s->tex.Get(), &dv, &s->dsv);
        sv.Format = DepthSrvFormat(fi.dxgi);
    } else {
        D3D11_RENDER_TARGET_VIEW_DESC rv{};
        rv.Format = fi.dxgi;
        rv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        dev_->CreateRenderTargetView(s->tex.Get(), &rv, &s->rtv);
        sv.Format = fi.dxgi;
    }
    dev_->CreateShaderResourceView(s->tex.Get(), &sv, &s->srv);
    LOG_DEBUG("Renderer", "New {} surface {:#x} {}x{} fmt {:#x}", depth ? "depth" : "color", gpu_va, width, height, format);
    Surface* raw = s.get();
    surfaces_[gpu_va] = std::move(s);
    return raw;
}

Surface* D3D11Renderer::GetColorSurface(u32 i) {
    const auto& m = G().M3D();
    const u32 base = kRenderTarget + i * 0x10;
    const u32 fmt = m.Reg(base + 4);
    if (fmt == 0)
        return nullptr;
    return GetSurface(m.Addr(base), m.Reg(base + 2), m.Reg(base + 3), fmt, false);
}

Surface* D3D11Renderer::GetDepthSurface() {
    const auto& m = G().M3D();
    if (!(m.Reg(kZetaEnable) & 1))
        return nullptr;
    return GetSurface(m.Addr(kZetaAddrHigh), m.Reg(kZetaWidth), m.Reg(kZetaHeight), m.Reg(kZetaFormat), true);
}

void D3D11Renderer::Clear(u32 rt, u32 mask) {
    std::lock_guard ctx_lock(ContextMutex());
    clears++;
    const auto& m = G().M3D();
    if (mask & 0x3C) {
        if (Surface* s = GetColorSurface(rt)) {
            const float c[4] = {m.RegF(kClearColor), m.RegF(kClearColor + 1), m.RegF(kClearColor + 2),
                                m.RegF(kClearColor + 3)};
            ctx_->ClearRenderTargetView(s->rtv.Get(), c);
        }
    }
    if (mask & 3) {
        if (Surface* s = GetDepthSurface()) {
            UINT flags = 0;
            if (mask & 1)
                flags |= D3D11_CLEAR_DEPTH;
            if (mask & 2)
                flags |= D3D11_CLEAR_STENCIL;
            ctx_->ClearDepthStencilView(s->dsv.Get(), flags, m.RegF(kClearDepth), u8(m.Reg(kClearStencil)));
        }
    }
}

void D3D11Renderer::Draw() {
    std::lock_guard ctx_lock(ContextMutex());
    auto& g = G();
    const auto& m = g.M3D();
    auto& dc = *g_dc;

    // ---- shaders
    const u64 code_base = m.Addr(kCodeAddrHigh);
    auto load_shader = [&](u32 program, shader::Stage stage) -> HostShader* {
        const u32 cfg = m.Reg(kShaderConfig + program * 0x10);
        if (!(cfg & 1) && program != 1) // VertexB is always enabled on Maxwell
            return nullptr;
        const u64 addr = code_base + m.Reg(kShaderConfig + program * 0x10 + 1);
        static thread_local std::vector<u8> buf;
        buf.assign(0x10000, 0);
        const size_t got = g.ReadGpuPartial(addr, buf.data(), buf.size());
        if (got < 0x60)
            return nullptr;
        return shaders_->Get(stage, buf.data(), got);
    };
    HostShader* vs = load_shader(1, shader::Stage::Vertex);
    if (!vs)
        vs = load_shader(0, shader::Stage::Vertex);
    HostShader* ps = load_shader(5, shader::Stage::Fragment);
    if (!vs || vs->state != HostShader::Ready || (ps && ps->state != HostShader::Ready)) {
        draws_skipped++;
        static int logged = 0;
        if ((!vs || vs->state == HostShader::Failed) && logged < 3) {
            logged++;
            LOG_WARN("Renderer", "Draw skipped: vs {} ps {}; programs:", (void*)vs, (void*)ps);
            for (u32 i = 0; i < 6; i++)
                LOG_WARN("Renderer", "  program {}: {:#x} offset {:#x}", i, m.Reg(kShaderConfig + i * 0x10),
                         m.Reg(kShaderConfig + i * 0x10 + 1));
        }
        return;
    }

    // ---- render targets
    Surface* colors[8]{};
    const u32 rtc = m.Reg(kRtControl);
    const u32 count = std::min<u32>(rtc & 0xF, 8);
    for (u32 i = 0; i < count; i++)
        colors[i] = GetColorSurface((rtc >> (4 + i * 3)) & 7);
    Surface* depth = GetDepthSurface();
    ID3D11RenderTargetView* rtvs[8]{};
    for (u32 i = 0; i < count; i++)
        rtvs[i] = colors[i] ? colors[i]->rtv.Get() : nullptr;
    ctx_->OMSetRenderTargets(8, rtvs, depth ? depth->dsv.Get() : nullptr);

    // ---- viewport
    const float sx = m.RegF(kViewportTransform + 0), sy = m.RegF(kViewportTransform + 1);
    const float sz = m.RegF(kViewportTransform + 2), tx = m.RegF(kViewportTransform + 3);
    const float ty = m.RegF(kViewportTransform + 4), tz = m.RegF(kViewportTransform + 5);
    D3D11_VIEWPORT vp{tx - std::abs(sx), ty - std::abs(sy), 2 * std::abs(sx), 2 * std::abs(sy), 0.0f, 1.0f};
    ctx_->RSSetViewports(1, &vp);
    float flip = sy > 0 ? -1.0f : 1.0f;
    if (m.Reg(kScreenYControl) & 1)
        flip = -flip;

    // ---- scissor
    const u32 sc_enable = m.Reg(0x380);
    D3D11_RECT sr{0, 0, 16384, 16384};
    if (sc_enable & 1) {
        const u32 x = m.Reg(0x381), y = m.Reg(0x382);
        sr = {LONG(x & 0xFFFF), LONG(y & 0xFFFF), LONG(x >> 16), LONG(y >> 16)};
    }
    ctx_->RSSetScissorRects(1, &sr);

    // ---- blend
    D3D11_BLEND_DESC bd{};
    bd.IndependentBlendEnable = TRUE;
    const bool independent = m.Reg(0x4B9) & 1;
    for (u32 i = 0; i < 8; i++) {
        auto& t = bd.RenderTarget[i];
        t.BlendEnable = m.Reg(kBlendEnable + i) & 1;
        u32 eq_rgb, src_rgb, dst_rgb, eq_a, src_a, dst_a;
        if (independent) {
            const u32 b = kIndependentBlend + i * 8;
            eq_rgb = m.Reg(b + 1), src_rgb = m.Reg(b + 2), dst_rgb = m.Reg(b + 3);
            eq_a = m.Reg(b + 4), src_a = m.Reg(b + 5), dst_a = m.Reg(b + 6);
        } else {
            eq_rgb = m.Reg(kBlendEqRgb), src_rgb = m.Reg(kBlendSrcRgb), dst_rgb = m.Reg(kBlendDstRgb);
            eq_a = m.Reg(kBlendEqA), src_a = m.Reg(kBlendSrcA), dst_a = m.Reg(kBlendDstA);
        }
        t.BlendOp = BlendOp(eq_rgb);
        t.SrcBlend = BlendFactor(src_rgb);
        t.DestBlend = BlendFactor(dst_rgb);
        t.BlendOpAlpha = BlendOp(eq_a);
        t.SrcBlendAlpha = AlphaFactor(BlendFactor(src_a));
        t.DestBlendAlpha = AlphaFactor(BlendFactor(dst_a));
        const u32 cm = m.Reg(kColorMask + i);
        t.RenderTargetWriteMask = u8(((cm & 1) ? 1 : 0) | ((cm & 0x10) ? 2 : 0) | ((cm & 0x100) ? 4 : 0) |
                                     ((cm & 0x1000) ? 8 : 0));
    }
    auto* blend = dc.Cached(dc.blend, bd, [&](const D3D11_BLEND_DESC* d, ID3D11BlendState** o) {
        dev_->CreateBlendState(d, o);
    });
    const float bc[4] = {m.RegF(kBlendColor), m.RegF(kBlendColor + 1), m.RegF(kBlendColor + 2), m.RegF(kBlendColor + 3)};
    ctx_->OMSetBlendState(blend, bc, 0xFFFFFFFF);

    // ---- depth / stencil
    D3D11_DEPTH_STENCIL_DESC dd{};
    dd.DepthEnable = depth && (m.Reg(kDepthTestEnable) & 1);
    dd.DepthWriteMask = (m.Reg(kDepthWriteEnable) & 1) ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
    dd.DepthFunc = CompareFunc(m.Reg(kDepthTestFunc));
    dd.StencilEnable = depth && (m.Reg(kStencilEnable) & 1);
    dd.StencilReadMask = u8(m.Reg(kStencilFrontFuncMask));
    dd.StencilWriteMask = u8(m.Reg(kStencilFrontMask));
    dd.FrontFace = {StencilOp(m.Reg(kStencilFrontOpFail)), StencilOp(m.Reg(kStencilFrontOpZFail)),
                    StencilOp(m.Reg(kStencilFrontOpZPass)), CompareFunc(m.Reg(kStencilFrontFunc))};
    dd.BackFace = (m.Reg(kStencilTwoSide) & 1)
                      ? D3D11_DEPTH_STENCILOP_DESC{StencilOp(m.Reg(kStencilBackOpFail)), StencilOp(m.Reg(kStencilBackOpZFail)),
                                                   StencilOp(m.Reg(kStencilBackOpZPass)), CompareFunc(m.Reg(kStencilBackFunc))}
                      : dd.FrontFace;
    auto* dss = dc.Cached(dc.depth, dd, [&](const D3D11_DEPTH_STENCIL_DESC* d, ID3D11DepthStencilState** o) {
        dev_->CreateDepthStencilState(d, o);
    });
    ctx_->OMSetDepthStencilState(dss, m.Reg(kStencilFrontRef) & 0xFF);

    // ---- rasterizer
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    if (m.Reg(kCullEnable) & 1) {
        const u32 cf = m.Reg(kCullFace);
        rd.CullMode = cf == 0x404 ? D3D11_CULL_FRONT : cf == 0x405 ? D3D11_CULL_BACK : D3D11_CULL_NONE;
    }
    bool ccw = m.Reg(kFrontFace) == 0x901;
    if (flip < 0)
        ccw = !ccw;
    rd.FrontCounterClockwise = ccw;
    rd.DepthClipEnable = TRUE;
    rd.ScissorEnable = TRUE;
    auto* rs = dc.Cached(dc.raster, rd, [&](const D3D11_RASTERIZER_DESC* d, ID3D11RasterizerState** o) {
        dev_->CreateRasterizerState(d, o);
    });
    ctx_->RSSetState(rs);

    // ---- topology / indices
    const auto& dr = m.draw;
    std::vector<u32> converted;
    u32 index_size = 0;
    u32 min_v = 0, max_v = 0;
    D3D11_PRIMITIVE_TOPOLOGY topo = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    std::vector<u32> indices;
    if (dr.indexed) {
        const u32 fmt = m.Reg(kIndexFormat);
        index_size = fmt == 0 ? 1 : fmt == 1 ? 2 : 4;
        std::vector<u8> raw(size_t(dr.count) * index_size);
        g.ReadGpu(m.Addr(kIndexAddrHigh) + u64(dr.first) * index_size, raw.data(), raw.size());
        indices.resize(dr.count);
        for (u32 i = 0; i < dr.count; i++)
            indices[i] = index_size == 1 ? raw[i] : index_size == 2 ? common::Read<u16>(&raw[i * 2])
                                                                   : common::Read<u32>(&raw[i * 4]);
    } else {
        indices.resize(dr.count);
        for (u32 i = 0; i < dr.count; i++)
            indices[i] = dr.first + i;
    }
    switch (dr.topology) {
    case 0: topo = D3D11_PRIMITIVE_TOPOLOGY_POINTLIST; break;
    case 1: topo = D3D11_PRIMITIVE_TOPOLOGY_LINELIST; break;
    case 3: topo = D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP; break;
    case 4: topo = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST; break;
    case 5: topo = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP; break;
    case 6: case 9: { // fan / polygon -> list
        std::vector<u32> t;
        for (u32 i = 2; i < indices.size(); i++)
            t.insert(t.end(), {indices[0], indices[i - 1], indices[i]});
        indices.swap(t);
        break;
    }
    case 7: { // quads
        std::vector<u32> t;
        for (u32 i = 0; i + 3 < indices.size(); i += 4)
            t.insert(t.end(), {indices[i], indices[i + 1], indices[i + 2], indices[i], indices[i + 2], indices[i + 3]});
        indices.swap(t);
        break;
    }
    case 8: { // quad strip
        std::vector<u32> t;
        for (u32 i = 0; i + 3 < indices.size(); i += 2)
            t.insert(t.end(), {indices[i], indices[i + 1], indices[i + 3], indices[i], indices[i + 3], indices[i + 2]});
        indices.swap(t);
        break;
    }
    case 2: // line loop
        if (!indices.empty())
            indices.push_back(indices[0]);
        topo = D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP;
        break;
    default:
        draws_skipped++;
        return;
    }
    if (indices.empty()) {
        draws_skipped++;
        return;
    }
    min_v = *std::min_element(indices.begin(), indices.end());
    max_v = *std::max_element(indices.begin(), indices.end());
    if (max_v - min_v > 4000000) {
        draws_skipped++;
        return;
    }
    ctx_->IASetPrimitiveTopology(topo);
    ctx_->IASetIndexBuffer(dc.Index(indices.data(), u32(indices.size() * 4)), DXGI_FORMAT_R32_UINT, 0);

    // ---- driver constants + vertex buffers
    DriverConstants drv{};
    drv.viewport[0] = flip;
    drv.viewport[1] = sz;
    drv.viewport[2] = tz;
    for (u32 i = 0; i < 32; i++)
        drv.attrib[i][0] = m.Reg(kVertexAttribFormat + i);
    // ---- vertex input: Maxwell attribute formats -> D3D11 input layout
    std::vector<D3D11_INPUT_ELEMENT_DESC> elems;
    bool used_buf[16]{};
    bool need_default = false;
    for (u32 i = 0; i < 32; i++) {
        if (!(vs->info.attrib_in_mask & (1u << i)))
            continue;
        const u32 f = m.Reg(kVertexAttribFormat + i);
        const u32 b = f & 0x1F;
        const bool constant = f & 0x40;
        const DXGI_FORMAT fmt = AttribFormat(f);
        const u32 cfg = b < 16 ? m.Reg(kVertexArray + b * 4) : 0;
        D3D11_INPUT_ELEMENT_DESC e{"ATTR", i, fmt, b, (f >> 7) & 0x3FFF, D3D11_INPUT_PER_VERTEX_DATA, 0};
        if (constant || fmt == DXGI_FORMAT_UNKNOWN || b >= 16 || !(cfg & 0x1000)) {
            e = {"ATTR", i, DXGI_FORMAT_R32G32B32A32_FLOAT, 31, 0, D3D11_INPUT_PER_VERTEX_DATA, 0};
            need_default = true;
        } else {
            const u32 divisor = m.Reg(kVertexArray + b * 4 + 3);
            if (divisor) {
                e.InputSlotClass = D3D11_INPUT_PER_INSTANCE_DATA;
                e.InstanceDataStepRate = divisor;
            }
            used_buf[b] = true;
        }
        elems.push_back(e);
    }
    ID3D11InputLayout* layout = nullptr;
    if (!elems.empty()) {
        u64 key = vs->hash;
        for (auto& e : elems)
            key = key * 31 + Hash64(&e.SemanticIndex, sizeof(e) - offsetof(D3D11_INPUT_ELEMENT_DESC, SemanticIndex));
        auto it = dc.layouts.find(key);
        if (it == dc.layouts.end()) {
            ComPtr<ID3D11InputLayout> l;
            const HRESULT hr = dev_->CreateInputLayout(elems.data(), UINT(elems.size()), vs->dxbc.data(), vs->dxbc.size(), &l);
            if (FAILED(hr))
                LOG_WARN("Renderer", "CreateInputLayout failed {:#x} (vs {:016x})", u32(hr), vs->hash);
            it = dc.layouts.emplace(key, l).first;
        }
        layout = it->second.Get();
        if (!layout) {
            draws_skipped++;
            return;
        }
    }
    ctx_->IASetInputLayout(layout);
    ID3D11Buffer* vbufs[32]{};
    UINT strides[32]{}, offsets[32]{};
    s32 base_vertex = 0;
    for (u32 b = 0; b < 16; b++) {
        if (!used_buf[b])
            continue;
        const u32 cfg = m.Reg(kVertexArray + b * 4);
        const u32 stride = cfg & 0xFFF;
        const u32 divisor = m.Reg(kVertexArray + b * 4 + 3);
        const u64 addr = m.Addr(kVertexArray + b * 4 + 1);
        const u64 limit = m.Addr(kVertexArrayLimit + b * 2);
        u64 begin, end;
        if (divisor) {
            begin = addr;
            end = std::min<u64>(limit + 1, addr + u64(stride) * (dr.instance / divisor + 1) + 64);
        } else {
            begin = addr + u64(min_v) * stride;
            end = std::min<u64>(limit + 1, addr + u64(max_v + 1) * stride + 64);
            base_vertex = -s32(min_v);
        }
        if (end <= begin)
            end = begin + 16;
        const u32 size = u32(std::min<u64>(end - begin, 64ull << 20));
        static thread_local std::vector<u8> tmp;
        tmp.assign(size_t(size) + 64, 0);
        g.ReadGpu(begin, tmp.data(), size);
        vbufs[b] = dc.Vertex(b, tmp.data(), u32(tmp.size()));
        strides[b] = stride;
    }
    if (need_default) {
        if (!dc.default_attrib) {
            const float v[4] = {0, 0, 0, 1};
            D3D11_BUFFER_DESC d{16, D3D11_USAGE_IMMUTABLE, D3D11_BIND_VERTEX_BUFFER, 0, 0, 0};
            D3D11_SUBRESOURCE_DATA sd{v, 0, 0};
            dev_->CreateBuffer(&d, &sd, &dc.default_attrib);
        }
        vbufs[31] = dc.default_attrib.Get();
        strides[31] = 0;
    }
    ctx_->IASetVertexBuffers(0, 32, vbufs, strides, offsets);
    drv.misc[0] = 0;
    drv.misc[1] = m.Reg(kAlphaTestFunc) >= 0x200 ? m.Reg(kAlphaTestFunc) - 0x200 + 1 : m.Reg(kAlphaTestFunc);
    drv.misc[2] = m.Reg(kAlphaTestRef);
    drv.misc[3] = m.Reg(kAlphaTestEnable) & 1;

    // Trace every draw into the presented surface once (debug aid, MUA3_TRACE_DRAWS).
    trace_draw_ = trace_draws_ > 0 && colors[0] && presented_cpu_.count(colors[0]->cpu_va);
    if (trace_draw_) {
        trace_draws_--;
        LOG_INFO("Renderer", "TRACE draw vs {:016x} ps {:016x} n {} rt0 {:#x} blend0 {} cmask {:#x} depth {}", vs->hash,
                 ps ? ps->hash : 0, indices.size(), colors[0]->gpu_va, m.Reg(kBlendEnable), m.Reg(kColorMask), depth ? 1 : 0);
    }
    // ---- per stage resources
    auto bind_stage = [&](HostShader* s, u32 stage_index, bool is_vs) {
        // constant buffers
        ID3D11Buffer* cbs[13]{};
        for (u32 i = 0; i < 13; i++) {
            if (!(s->info.cbuf_mask & (1u << i)))
                continue;
            const auto& b = m.CBuf(stage_index, i);
            if (!b.enabled || !b.size)
                continue;
            static thread_local std::vector<u8> tmp;
            const u32 size = u32(common::AlignUp(std::min<u32>(b.size, 65536), 16));
            tmp.resize(size);
            g.ReadGpu(b.address, tmp.data(), size);
            cbs[i] = dc.Constant(stage_index * 32 + i, tmp.data(), size);
        }
        // textures
        ID3D11ShaderResourceView* srvs[32]{};
        ID3D11SamplerState* smps[16]{};
        const u32 tex_cb = m.Reg(kTexCbIndex);
        const auto& tcb = m.CBuf(stage_index, tex_cb);
        const u64 tic_base = m.Addr(kTicAddrHigh), tsc_base = m.Addr(kTscAddrHigh);
        for (auto& t : s->info.textures) {
            if (t.slot >= 32)
                continue;
            u32 handle = 0;
            g.ReadGpu(tcb.address + t.cbuf_offset, &handle, 4);
            const u32 tic_idx = handle & 0xFFFFF, tsc_idx = (handle >> 20) & 0xFFF;
            TicEntry tic;
            g.ReadGpu(tic_base + u64(tic_idx) * 32, tic.w.data(), 32);
            u32 tsc[8];
            g.ReadGpu(tsc_base + u64(tsc_idx) * 32, tsc, 32);
            u32 swz = 0xB1A;
            srvs[t.slot] = textures_->Get(tic, t, swz);
            NoteTexture(srvs[t.slot], tic.Address(), tic.Width(), tic.Height(), tic.Format(),
                        FindSurfaceByGpu(tic.Address()) != nullptr);
            if (trace_draw_)
                LOG_INFO("Renderer", "    {} tex slot {} handle {:#x} tic {} addr {:#x} fmt {:#x} type {} {}x{} {}", is_vs ? "VS" : "PS",
                         t.slot, handle, tic_idx, tic.Address(), tic.Format(), tic.Type(), tic.Width(), tic.Height(),
                         FindSurfaceByGpu(tic.Address()) ? "(render target)" : "");
            drv.swizzle[t.slot][0] = swz;
            if (t.slot < 16)
                smps[t.slot] = textures_->GetSampler(tsc, t.shadow);
        }
        if (is_vs) {
            ctx_->VSSetConstantBuffers(0, 13, cbs);
            ctx_->VSSetShaderResources(0, 32, srvs);
            ctx_->VSSetSamplers(0, 16, smps);
        } else {
            ctx_->PSSetConstantBuffers(0, 13, cbs);
            ctx_->PSSetShaderResources(0, 32, srvs);
            ctx_->PSSetSamplers(0, 16, smps);
        }
    };
    // Unbind surfaces that may also be used as textures this draw (D3D forbids read/write hazards).
    bind_stage(vs, 0, true);
    if (ps)
        bind_stage(ps, 4, false);

    ID3D11Buffer* drv_cb = dc.Constant(1000, &drv, sizeof(drv));
    ctx_->VSSetConstantBuffers(13, 1, &drv_cb);
    ctx_->PSSetConstantBuffers(13, 1, &drv_cb);

    ctx_->VSSetShader(vs->vs.Get(), nullptr, 0);
    ctx_->PSSetShader(ps ? ps->ps.Get() : nullptr, nullptr, 0);
    if (debug_mode_ >= 1) { // MUA3_DEBUG_PS=1: solid magenta; 2: real shaders; both without blend/depth
        if (debug_mode_ == 1)
            ctx_->PSSetShader(debug_ps_.Get(), nullptr, 0);
        ctx_->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
        ctx_->OMSetDepthStencilState(nullptr, 0);
    }
    ctx_->GSSetShader(nullptr, nullptr, 0);

    static int traced = 0;
    if (traced < 12) {
        traced++;
        LOG_INFO("Renderer", "DRAW vs {:016x} ps {:016x} topo {} idx {} n {} [{}..{}] rt0 {:#x} fmt {:#x} {}x{} vp ({},{} {}x{}) flip {} sz {} tz {}",
                 vs->hash, ps ? ps->hash : 0, dr.topology, dr.indexed, indices.size(), min_v, max_v,
                 colors[0] ? colors[0]->gpu_va : 0, colors[0] ? colors[0]->format : 0, colors[0] ? colors[0]->width : 0,
                 colors[0] ? colors[0]->height : 0, vp.TopLeftX, vp.TopLeftY, vp.Width, vp.Height, flip, sz, tz);
        LOG_INFO("Renderer", "     scissor en {} ({},{})-({},{}) cmask0 {:#x} blend0 {} rtc {:#x} depth {} attr0 {:#x} attr1 {:#x} vb0 {:#x} {:#x}",
                 sc_enable & 1, sr.left, sr.top, sr.right, sr.bottom, m.Reg(kColorMask), m.Reg(kBlendEnable), rtc,
                 depth ? 1 : 0, m.Reg(kVertexAttribFormat), m.Reg(kVertexAttribFormat + 1), m.Reg(kVertexArray),
                 m.Addr(kVertexArray + 1));
    }
    ctx_->DrawIndexedInstanced(u32(indices.size()), 1, 0, base_vertex, dr.instance);
    draws_ok++;

    // Release texture bindings so render targets can be re-bound as outputs.
    ID3D11ShaderResourceView* null_srv[32]{};
    ctx_->PSSetShaderResources(0, 32, null_srv);
    ctx_->VSSetShaderResources(0, 32, null_srv);
}

void D3D11Renderer::Present(const PresentInfo& p) {
    std::lock_guard ctx_lock(ContextMutex());
    frame_counter_++;
    {
        // D3D debug layer messages (only present when MUA3_D3D_DEBUG is set)
        ComPtr<ID3D11InfoQueue> q;
        if (SUCCEEDED(dev_->QueryInterface(__uuidof(ID3D11InfoQueue), (void**)&q))) {
            const UINT64 n = q->GetNumStoredMessages();
            for (UINT64 i = 0; i < n && i < 20; i++) {
                SIZE_T len = 0;
                q->GetMessage(i, nullptr, &len);
                std::vector<u8> buf(len);
                auto* msg = reinterpret_cast<D3D11_MESSAGE*>(buf.data());
                q->GetMessage(i, msg, &len);
                LOG_WARN("D3D11", "{}", std::string(msg->pDescription, msg->DescriptionByteLength));
            }
            q->ClearStoredMessages();
        }
    }
    textures_->NextFrame();
    Surface* s = FindSurfaceByCpu(p.cpu_addr);
    last_present_cpu_ = p.cpu_addr;
    presented_cpu_.insert(p.cpu_addr);
    if (dump_surfaces_.exchange(false)) {
        int n = 0;
        for (auto& [k, sf] : surfaces_)
            if (!sf->depth)
                DumpSurface(sf.get(), fmt::format("screenshots/debug3/surf_{:02}_{:x}_{}x{}_{:x}{}.bmp", n++, sf->gpu_va,
                                                  sf->width, sf->height, sf->format, sf.get() == s ? "_present" : ""));
    }
    if (frame_counter_ % 150 == 1) {
        LOG_INFO("Renderer", "Present cpu {:#x} {}x{}: surface {} | draws ok {} skipped {} clears {} surfaces {} shaders {} (failed {})",
                 p.cpu_addr, p.width, p.height, s ? fmt::format("{:#x}", s->gpu_va) : "none", draws_ok, draws_skipped,
                 clears, surfaces_.size(), shaders_->Count(), shaders_->failures.load());
        if (!s)
            for (auto& [k, sf] : surfaces_)
                LOG_INFO("Renderer", "  surface gpu {:#x} cpu {:#x} {}x{} fmt {:#x}{}", sf->gpu_va, sf->cpu_va, sf->width,
                         sf->height, sf->format, sf->depth ? " depth" : "");
    }
    std::lock_guard lk(frame_mutex_);
    if (s) {
        D3D11_TEXTURE2D_DESC desc;
        s->tex->GetDesc(&desc);
        if (!frame_tex_ || frame_w_ != s->width || frame_h_ != s->height) {
            frame_tex_.Reset();
            frame_srv_.Reset();
            D3D11_TEXTURE2D_DESC td = desc;
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            dev_->CreateTexture2D(&td, nullptr, &frame_tex_);
            D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
            sv.Format = s->dxgi == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : s->dxgi;
            sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            sv.Texture2D.MipLevels = 1;
            dev_->CreateShaderResourceView(frame_tex_.Get(), &sv, &frame_srv_);
            frame_w_ = s->width;
            frame_h_ = s->height;
        }
        ctx_->CopyResource(frame_tex_.Get(), s->tex.Get());
    }
    ctx_->Flush();
}

// Debug: writes a colour surface to a .bmp (RGBA8/BGRA8/RGBA16F/R11G11B10 supported).
void D3D11Renderer::DumpSurface(Surface* s, const fs::path& path) {
    D3D11_TEXTURE2D_DESC td;
    s->tex->GetDesc(&td);
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> st;
    if (FAILED(dev_->CreateTexture2D(&td, nullptr, &st)))
        return;
    ctx_->CopyResource(st.Get(), s->tex.Get());
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ctx_->Map(st.Get(), 0, D3D11_MAP_READ, 0, &m)))
        return;
    std::vector<u8> bgr(size_t(s->width) * s->height * 3);
    auto f16 = [](u16 h) {
        const u32 e = (h >> 10) & 0x1F, mnt = h & 0x3FF;
        float v = e == 0 ? mnt / 1024.0f / 16384.0f : std::ldexp(1.0f + mnt / 1024.0f, int(e) - 15);
        return (h & 0x8000) ? -v : v;
    };
    for (u32 y = 0; y < s->height; y++) {
        const u8* row = static_cast<const u8*>(m.pData) + size_t(y) * m.RowPitch;
        for (u32 x = 0; x < s->width; x++) {
            float r = 0, g = 0, b = 0;
            switch (s->dxgi) {
            case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
                r = row[x * 4] / 255.f, g = row[x * 4 + 1] / 255.f, b = row[x * 4 + 2] / 255.f; break;
            case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: case DXGI_FORMAT_B8G8R8X8_UNORM:
                b = row[x * 4] / 255.f, g = row[x * 4 + 1] / 255.f, r = row[x * 4 + 2] / 255.f; break;
            case DXGI_FORMAT_R16G16B16A16_FLOAT: {
                const u16* p = reinterpret_cast<const u16*>(row) + x * 4;
                r = f16(p[0]), g = f16(p[1]), b = f16(p[2]);
                break;
            }
            case DXGI_FORMAT_R11G11B10_FLOAT: {
                const u32 v = reinterpret_cast<const u32*>(row)[x];
                r = f16(u16((v & 0x7FF) << 4)), g = f16(u16(((v >> 11) & 0x7FF) << 4)),
                b = f16(u16(((v >> 22) & 0x3FF) << 5));
                break;
            }
            default: r = row[x * 4] / 255.f; break;
            }
            u8* o = &bgr[(size_t(s->height - 1 - y) * s->width + x) * 3];
            o[0] = u8(std::clamp(b, 0.f, 1.f) * 255), o[1] = u8(std::clamp(g, 0.f, 1.f) * 255),
            o[2] = u8(std::clamp(r, 0.f, 1.f) * 255);
        }
    }
    ctx_->Unmap(st.Get(), 0);
    const u32 row_size = (s->width * 3 + 3) & ~3u;
    std::vector<u8> file(54 + size_t(row_size) * s->height);
    auto put32 = [&](size_t o, u32 v) { std::memcpy(&file[o], &v, 4); };
    file[0] = 'B', file[1] = 'M';
    put32(2, u32(file.size())), put32(10, 54), put32(14, 40), put32(18, s->width), put32(22, s->height);
    file[26] = 1, file[28] = 24;
    for (u32 y = 0; y < s->height; y++)
        std::memcpy(&file[54 + size_t(y) * row_size], &bgr[size_t(y) * s->width * 3], size_t(s->width) * 3);
    common::WriteFile(path, file);
}

void D3D11Renderer::NoteTexture(ID3D11ShaderResourceView* srv, u64 addr, u32 w, u32 h, u32 fmt, bool rt) {
    if (!srv)
        return;
    std::lock_guard lk(recent_mutex_);
    for (auto& r : recent_)
        if (r.addr == addr)
            return;
    recent_.insert(recent_.begin(), RecentTexture{srv, addr, w, h, fmt, rt});
    if (recent_.size() > 48)
        recent_.pop_back();
}

ID3D11ShaderResourceView* D3D11Renderer::LatestFrame(u32& w, u32& h) {
    w = frame_w_;
    h = frame_h_;
    return frame_srv_.Get();
}

void D3D11Renderer::InvalidateRegion(u64 gpu_va, u64 size) { textures_->Invalidate(gpu_va, size); }

bool D3D11Renderer::Blit2D(const u32* regs) { return false; }

void D3D11Renderer::FlushCommands() {}

void D3D11Renderer::BindRenderTargets(Surface**, u32, Surface*) {}

} // namespace gpu
