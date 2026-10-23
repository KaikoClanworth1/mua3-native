#include "gpu/d3d11/renderer_d3d11.h"
#include "gpu/shader/maxwell_isa.h"

#include <set>

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
// 1 for UINT, 2 for SINT DXGI formats, 0 otherwise.
int DxgiIntKind(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R32G32B32A32_UINT: case DXGI_FORMAT_R32G32B32_UINT: case DXGI_FORMAT_R16G16B16A16_UINT:
    case DXGI_FORMAT_R32G32_UINT: case DXGI_FORMAT_R10G10B10A2_UINT: case DXGI_FORMAT_R8G8B8A8_UINT:
    case DXGI_FORMAT_R16G16_UINT: case DXGI_FORMAT_R32_UINT: case DXGI_FORMAT_R8G8_UINT: case DXGI_FORMAT_R16_UINT:
    case DXGI_FORMAT_R8_UINT:
        return 1;
    case DXGI_FORMAT_R32G32B32A32_SINT: case DXGI_FORMAT_R32G32B32_SINT: case DXGI_FORMAT_R16G16B16A16_SINT:
    case DXGI_FORMAT_R32G32_SINT: case DXGI_FORMAT_R8G8B8A8_SINT: case DXGI_FORMAT_R16G16_SINT: case DXGI_FORMAT_R32_SINT:
    case DXGI_FORMAT_R8G8_SINT: case DXGI_FORMAT_R16_SINT: case DXGI_FORMAT_R8_SINT:
        return 2;
    default:
        return 0;
    }
}

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
    // D3D11 has no signed 10:10:10:2: SNORM/SINT come in as raw UINT bits (the VS variant sign-extends SNORM).
    case 0x30: return type == 2 ? DXGI_FORMAT_R10G10B10A2_UNORM : DXGI_FORMAT_R10G10B10A2_UINT;
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
    u32 gmem[8][4]; // compute global memory windows: base lo, base hi, size
    u32 image[8][4]; // compute images: base lo, base hi, width | height << 16, depth | mode << 16 | bh << 24 | bd << 28
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

Surface* D3D11Renderer::GetSurface(u64 gpu_va, u32 width, u32 height, u32 format, bool depth, u32 tile_mode) {
    if (!gpu_va || !width || !height || width > 16384 || height > 16384)
        return nullptr;
    auto it = surfaces_.find(gpu_va);
    if (it != surfaces_.end()) {
        Surface* s = it->second.get();
        if (s->width == width && s->height == height && s->format == format && s->depth == depth) {
            s->last_use = frame_counter_;
            return s;
        }
        LOG_DEBUG("Renderer", "Surface {:#x} redefined: {}x{} fmt {:#x} -> {}x{} fmt {:#x} (contents dropped)", gpu_va,
                  s->width, s->height, s->format, width, height, format);
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
    // A render target is guest memory on the Switch: start from what the guest left there (e.g. values the CPU
    // seeded), deswizzled from the block-linear layout. Pitch-linear targets just start cleared.
    std::vector<u8> initial;
    D3D11_SUBRESOURCE_DATA init{};
    const bool pitch_linear = (tile_mode >> 12) & 1;
    if (!depth && !pitch_linear && s->cpu_va) {
        const u32 bh = (tile_mode >> 4) & 0xF;
        const u64 size = BlockLinearSize(width, height, 1, fi.bpp, bh, 0);
        std::vector<u8> raw(size_t(size), 0);
        if (G().ReadGpuPartial(gpu_va, raw.data(), raw.size()) == raw.size()) {
            initial.resize(size_t(width) * height * fi.bpp);
            Deswizzle(raw.data(), initial.data(), width, height, 1, fi.bpp, bh, 0, width * fi.bpp);
            init.pSysMem = initial.data();
            init.SysMemPitch = width * fi.bpp;
        }
    }
    if (FAILED(dev_->CreateTexture2D(&td, init.pSysMem ? &init : nullptr, &s->tex))) {
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

ID3D11ShaderResourceView* D3D11Renderer::HazardSrv(Surface* s) {
    if (std::find(std::begin(bound_), std::end(bound_), s) == std::end(bound_))
        return nullptr;
    if (!s->copy_tex) {
        D3D11_TEXTURE2D_DESC td;
        s->tex->GetDesc(&td);
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SHADER_RESOURCE_VIEW_DESC sv;
        s->srv->GetDesc(&sv);
        if (FAILED(dev_->CreateTexture2D(&td, nullptr, &s->copy_tex)) ||
            FAILED(dev_->CreateShaderResourceView(s->copy_tex.Get(), &sv, &s->copy_srv))) {
            s->copy_tex.Reset();
            return nullptr;
        }
    }
    if (s->copy_serial != draw_serial_) {
        ctx_->CopyResource(s->copy_tex.Get(), s->tex.Get());
        s->copy_serial = draw_serial_;
    }
    return s->copy_srv.Get();
}

Surface* D3D11Renderer::GetColorSurface(u32 i) {
    const auto& m = G().M3D();
    const u32 base = kRenderTarget + i * 0x10;
    const u32 fmt = m.Reg(base + 4);
    if (fmt == 0)
        return nullptr;
    return GetSurface(m.Addr(base), m.Reg(base + 2), m.Reg(base + 3), fmt, false, m.Reg(base + 5));
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

// MUA3's culling geometry program: for each visible triangle it atomically reserves three slots in an index list
// (counter address at GS c0[0x410], list base at c0[0x420]) and stores the vertex ids as v1, v0, v2. Later draws
// index that list. We keep every triangle (culling is only an optimisation).
static_assert(shader::kGmemWindows == 8, "D3D11Renderer::gmem_ size");

void D3D11Renderer::Dispatch(const u32* r, const u32* q) {
    std::lock_guard ctx_lock(ContextMutex());
    auto& g = G();
    auto& dc = *g_dc;
    // ---- program and launch shape
    const u64 code = (u64(r[0x582] & 0xFF) << 32) | r[0x583];
    const u64 prog = code + q[8];
    shader::TranslateOptions opt;
    opt.block[0] = std::max<u32>(q[0x12] >> 16, 1);
    opt.block[1] = std::max<u32>(q[0x13] & 0xFFFF, 1);
    opt.block[2] = std::max<u32>(q[0x13] >> 16, 1);
    opt.shared_bytes = q[0x11] & 0x3FFFF;
    const u32 grid[3] = {q[0xC] & 0x7FFFFFFF, q[0xD] & 0xFFFF, q[0xD] >> 16};
    if (!grid[0] || !grid[1] || !grid[2])
        return;
    // Debug: MUA3_SKIP_CS=<offset hex>,... drops those compute programs.
    static const std::string skip_cs = [] {
        const char* e = std::getenv("MUA3_SKIP_CS");
        return std::string(e ? e : "");
    }();
    if (!skip_cs.empty() && skip_cs.find(fmt::format("{:x}", q[8])) != std::string::npos)
        return;
    const u64 key = prog ^ (u64(opt.block[0]) << 40) ^ (u64(opt.block[1]) << 50) ^ (u64(opt.block[2]) << 56) ^
                    (u64(opt.shared_bytes) << 20);
    HostShader* cs;
    if (auto it = compute_by_key_.find(key); it != compute_by_key_.end()) {
        cs = it->second;
    } else {
        // Compute programs have no SPH: put them behind a zeroed one like the graphics stages expect.
        std::vector<u8> buf(0x50 + 0x40000, 0);
        const size_t got = g.ReadGpuPartial(prog, buf.data() + 0x50, 0x40000);
        if (got < 0x10)
            return;
        std::vector<u8> cb1;
        if (q[0x14] & 2) { // jump tables for BRX
            const u64 a = (u64(q[0x20] & 0xFF) << 32) | q[0x1F];
            cb1.assign(std::min<u32>(q[0x20] >> 15, 0x10000), 0);
            g.ReadGpuPartial(a, cb1.data(), cb1.size());
            opt.cbuf1 = cb1.data();
            opt.cbuf1_size = u32(cb1.size());
        }
        cs = shaders_->Get(shader::Stage::Compute, buf.data(), 0x50 + got, &opt);
        opt.cbuf1 = nullptr;
        opt.cbuf1_size = 0;
        compute_by_key_[key] = cs;
        code_lo_ = std::min(code_lo_, prog);
        code_hi_ = std::max(code_hi_, prog + got);
    }
    if (cs->state != HostShader::Ready)
        return;
    // ---- constant buffers (QMD slots)
    const u32 cb_mask = q[0x14] & 0xFF;
    u64 cb_addr[8]{};
    u32 cb_size[8]{};
    for (u32 i = 0; i < 8; i++)
        if (cb_mask & (1u << i)) {
            cb_addr[i] = (u64(q[0x1E + i * 2] & 0xFF) << 32) | q[0x1D + i * 2];
            cb_size[i] = q[0x1E + i * 2] >> 15;
        }
    // Integer-typed textures need a shader variant, as for draws.
    const u64 tic_base = (u64(r[0x55D] & 0xFF) << 32) | r[0x55E];
    const u64 tsc_base = (u64(r[0x557] & 0xFF) << 32) | r[0x558];
    const u32 tex_cb = r[0x982] & 7;
    auto handle_of = [&](const shader::TextureUse& t) {
        u32 h = 0;
        g.ReadGpu(cb_addr[tex_cb] + t.cbuf_offset, &h, 4);
        return h;
    };
    {
        ShaderCache::VariantKey vk;
        for (auto& t : cs->info.textures) {
            if (t.slot >= 32)
                continue;
            TicEntry tic;
            g.ReadGpu(tic_base + u64(handle_of(t) & 0xFFFFF) * 32, tic.w.data(), 32);
            if (FindSurfaceByGpu(tic.Address()))
                continue;
            switch (DxgiIntKind(TicFormat(tic.Format(), tic.RType(), tic.Srgb()).dxgi)) {
            case 1: vk.tex_uint |= 1u << t.slot; break;
            case 2: vk.tex_sint |= 1u << t.slot; break;
            default: break;
            }
        }
        cs = shaders_->GetVariant(cs, vk);
        if (cs->state != HostShader::Ready)
            return;
    }
    static std::unordered_map<u64, int> logged;
    const int nth = logged[key]++;
    const bool log = nth < 2 || nth % 600 == 0;
    if (log)
        LOG_INFO("Renderer", "dispatch {:#x} cs {:016x} grid {}x{}x{} block {}x{}x{} shared {:#x}", prog, cs->hash, grid[0], grid[1],
                 grid[2], opt.block[0], opt.block[1], opt.block[2], opt.shared_bytes);
    ID3D11Buffer* cbs[13]{};
    ID3D11ShaderResourceView* srvs[48]{};
    for (u32 i = 0; i < 8; i++) {
        if (!(cs->info.cbuf_mask & (1u << i)) || !cb_size[i])
            continue;
        static thread_local std::vector<u8> tmp;
        const bool dyn = (cs->info.cbuf_dyn_mask >> i) & 1;
        const u32 need = dyn ? 65536u : cs->info.cbuf_rows[i] * 16u;
        const u32 size = u32(common::AlignUp(std::max<u32>(std::min<u32>({cb_size[i], 65536u, need}), 16u), 16));
        tmp.assign(size, 0);
        g.ReadGpu(cb_addr[i], tmp.data(), std::min<u32>(size, cb_size[i]));
        cbs[i] = dc.Constant(5 * 32 + i, tmp.data(), size);
        if (dyn)
            srvs[32 + i] = dc.RawBuffer(5 * 32 + i, tmp.data(), size);
    }
    // ---- textures
    DriverConstants drv{};
    ID3D11SamplerState* smps[16]{};
    for (auto& t : cs->info.textures) {
        if (t.slot >= 32)
            continue;
        const u32 h = handle_of(t);
        TicEntry tic;
        g.ReadGpu(tic_base + u64(h & 0xFFFFF) * 32, tic.w.data(), 32);
        u32 tsc[8];
        g.ReadGpu(tsc_base + u64((h >> 20) & 0xFFF) * 32, tsc, 32);
        u32 swz = 0xB1A;
        srvs[t.slot] = textures_->Get(tic, t, swz);
        drv.swizzle[t.slot][0] = swz;
        if (t.sampler < 16)
            smps[t.sampler] = textures_->GetSampler(tsc, t.shadow);
    }
    // ---- global memory windows: NVN's storage buffer table in c0 at 0x310 ({address lo, hi, size, -} each)
    ID3D11UnorderedAccessView* uavs[shader::kGmemWindows]{};
    u64 win_addr[shader::kGmemWindows]{};
    u32 win_size[shader::kGmemWindows]{};
    if (cs->info.uses_gmem && cb_size[0] >= 0x310 + 16 * shader::kGmemWindows) {
        u32 table[shader::kGmemWindows][4];
        g.ReadGpu(cb_addr[0] + 0x310, table, sizeof(table));
        for (u32 i = 0; i < shader::kGmemWindows; i++) {
            const u64 a = (u64(table[i][1]) << 32) | table[i][0];
            const u32 size = u32(common::AlignUp(table[i][2], 4));
            if (!a || !size || size > (64u << 20))
                continue;
            GmemWindow& w = gmem_[i];
            if (w.capacity < size) {
                w = {};
                w.capacity = u32(common::AlignUp(size, 1u << 16));
                D3D11_BUFFER_DESC bd{w.capacity, D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS, 0,
                                     D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS, 0};
                dev_->CreateBuffer(&bd, nullptr, &w.buf);
                D3D11_BUFFER_DESC sd{w.capacity, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0, 0};
                dev_->CreateBuffer(&sd, nullptr, &w.staging);
                D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
                ud.Format = DXGI_FORMAT_R32_TYPELESS;
                ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
                ud.Buffer = {0, w.capacity / 4, D3D11_BUFFER_UAV_FLAG_RAW};
                dev_->CreateUnorderedAccessView(w.buf.Get(), &ud, &w.uav);
            }
            w.uploaded.assign(size, 0);
            g.ReadGpuPartial(a, w.uploaded.data(), size);
            const D3D11_BOX box{0, 0, 0, size, 1, 1};
            ctx_->UpdateSubresource(w.buf.Get(), 0, &box, w.uploaded.data(), size, size);
            uavs[i] = w.uav.Get();
            win_addr[i] = a;
            win_size[i] = size;
            drv.gmem[i][0] = u32(a);
            drv.gmem[i][1] = u32(a >> 32);
            drv.gmem[i][2] = size;
            if (log)
                LOG_INFO("Renderer", "  gmem window {}: {:#x} size {:#x}", i, a, size);
        }
    }
    // ---- images (SUST): mapped as extra global memory windows over their guest storage
    u32 next_window = 0;
    while (next_window < shader::kGmemWindows && win_size[next_window])
        next_window++;
    for (u32 k = 0; k < cs->info.images.size() && k < 8; k++) {
        u32 h = 0;
        g.ReadGpu(cb_addr[tex_cb] + cs->info.images[k], &h, 4);
        TicEntry tic;
        g.ReadGpu(tic_base + u64(h & 0xFFFFF) * 32, tic.w.data(), 32);
        const u32 fmt = tic.Format(), rt = tic.RType();
        const u32 mode = fmt == 0x03 ? 1u : fmt == 0x01 ? 2u : (fmt == 0x08 && rt == 2) ? 3u : fmt == 0x0F ? 4u
                       : fmt == 0x0C && rt == 7 ? 5u : fmt == 0x04 ? 6u : 0u;
        const u32 bpb = mode == 2 ? 16 : (mode == 1 || mode == 6) ? 8 : 4;
        const u32 w = tic.Width(), hgt = tic.Type() == 0 ? 1 : tic.Height(), dep = tic.Type() == 2 ? tic.Depth() : 1;
        // Same small-image block shrinking as the texture cache's level 0.
        u32 bh = tic.BlockHeight(), bd = tic.BlockDepth();
        while (bh > 0 && (8u << (bh - 1)) >= hgt)
            bh--;
        while (bd > 0 && (1u << (bd - 1)) >= dep)
            bd--;
        const u64 size = BlockLinearSize(w, hgt, dep, bpb, bh, bd);
        if (!mode || next_window >= shader::kGmemWindows || size > (64u << 20)) {
            static std::set<u32> warned;
            if (warned.insert(fmt | (rt << 8)).second)
                LOG_WARN("Renderer", "compute image store: unsupported format {:#x}/{} or no free window", fmt, rt);
            continue;
        }
        const u64 a = tic.Address();
        const u32 i = next_window++;
        GmemWindow& gw = gmem_[i];
        if (gw.capacity < size) {
            gw = {};
            gw.capacity = u32(common::AlignUp(size, 1u << 16));
            D3D11_BUFFER_DESC bd_{gw.capacity, D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS, 0,
                                  D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS, 0};
            dev_->CreateBuffer(&bd_, nullptr, &gw.buf);
            D3D11_BUFFER_DESC sd{gw.capacity, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0, 0};
            dev_->CreateBuffer(&sd, nullptr, &gw.staging);
            D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
            ud.Format = DXGI_FORMAT_R32_TYPELESS;
            ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
            ud.Buffer = {0, gw.capacity / 4, D3D11_BUFFER_UAV_FLAG_RAW};
            dev_->CreateUnorderedAccessView(gw.buf.Get(), &ud, &gw.uav);
        }
        gw.uploaded.assign(size_t(size), 0);
        g.ReadGpuPartial(a, gw.uploaded.data(), size_t(size));
        const D3D11_BOX box{0, 0, 0, u32(size), 1, 1};
        ctx_->UpdateSubresource(gw.buf.Get(), 0, &box, gw.uploaded.data(), u32(size), u32(size));
        uavs[i] = gw.uav.Get();
        win_addr[i] = a;
        win_size[i] = u32(size);
        drv.gmem[i][0] = u32(a);
        drv.gmem[i][1] = u32(a >> 32);
        drv.gmem[i][2] = u32(size);
        drv.image[k][0] = u32(a);
        drv.image[k][1] = u32(a >> 32);
        drv.image[k][2] = w | (hgt << 16);
        drv.image[k][3] = dep | (mode << 16) | (bh << 24) | (bd << 28);
    }
    // Debug: MUA3_CAPTURE=<program offset hex> saves one dispatch's inputs for `shadertool --replay`.
    static const u32 capture_off = [] {
        const char* e = std::getenv("MUA3_CAPTURE");
        return e ? u32(std::strtoul(e, nullptr, 16)) : ~0u;
    }();
    static bool captured = false;
    if (!captured && q[8] == capture_off) {
        captured = true;
        std::vector<u32> f;
        auto put_bytes = [&](const void* p, u32 n) {
            f.push_back(n);
            const u32 words = (n + 3) / 4;
            const size_t at = f.size();
            f.resize(at + words, 0);
            std::memcpy(&f[at], p, n);
        };
        f.push_back(0x5041434D); // "MCAP"
        put_bytes(cs->info.hlsl.data(), u32(cs->info.hlsl.size()));
        f.insert(f.end(), {opt.block[0], opt.block[1], opt.block[2], grid[0], grid[1], grid[2]});
        std::vector<u32> cbl;
        for (u32 i = 0; i < 8; i++)
            if (cb_size[i])
                cbl.push_back(i);
        f.push_back(u32(cbl.size()));
        for (u32 i : cbl) {
            std::vector<u8> d(std::min<u32>(cb_size[i], 65536u), 0);
            g.ReadGpuPartial(cb_addr[i], d.data(), d.size());
            f.push_back(i);
            put_bytes(d.data(), u32(d.size()));
        }
        std::vector<std::pair<u32, std::vector<u8>>> texs;
        std::vector<u32> tex_fmt;
        for (auto& t : cs->info.textures) {
            if (t.dim != shader::TexDim::Buffer)
                continue;
            TicEntry tic;
            g.ReadGpu(tic_base + u64(handle_of(t) & 0xFFFFF) * 32, tic.w.data(), 32);
            const TexFormatInfo fi = TicFormat(tic.Format(), tic.RType(), tic.Srgb());
            std::vector<u8> d(size_t(tic.Width()) * fi.bytes_per_block, 0);
            g.ReadGpuPartial(tic.Address(), d.data(), d.size());
            texs.push_back({t.slot, std::move(d)});
            tex_fmt.push_back(u32(fi.dxgi) | (fi.bytes_per_block << 16));
        }
        f.push_back(u32(texs.size()));
        for (size_t k = 0; k < texs.size(); k++) {
            f.push_back(texs[k].first);
            f.push_back(tex_fmt[k]);
            put_bytes(texs[k].second.data(), u32(texs[k].second.size()));
        }
        u32 nwin = 0;
        for (u32 i = 0; i < shader::kGmemWindows; i++)
            nwin += win_size[i] != 0;
        f.push_back(nwin);
        for (u32 i = 0; i < shader::kGmemWindows; i++)
            if (win_size[i]) {
                f.push_back(i);
                put_bytes(gmem_[i].uploaded.data(), win_size[i]);
            }
        put_bytes(&drv, sizeof(drv));
        const fs::path out = fmt::format("shader_dump/capture_{:x}.bin", q[8]);
        common::WriteFile(out, std::span(reinterpret_cast<const u8*>(f.data()), f.size() * 4));
        LOG_INFO("Renderer", "captured dispatch {:#x} to {}", prog, out.string());
    }
    ID3D11Buffer* drv_cb = dc.Constant(1001, &drv, sizeof(drv));
    // Render targets may be sampled by the kernel: unbind them from the output stage.
    ctx_->OMSetRenderTargets(0, nullptr, nullptr);
    ctx_->CSSetShader(cs->cs.Get(), nullptr, 0);
    ctx_->CSSetConstantBuffers(0, 13, cbs);
    ctx_->CSSetConstantBuffers(13, 1, &drv_cb);
    ctx_->CSSetShaderResources(0, 48, srvs);
    ctx_->CSSetSamplers(0, 16, smps);
    ctx_->CSSetUnorderedAccessViews(0, shader::kGmemWindows, uavs, nullptr);
    // MUA3_DXBC_VERIFY: run the FXC build first on the same inputs, keep its results, restore the inputs, then run
    // the direct-DXBC build; the write-back below compares the two.
    std::vector<std::vector<u8>> reference(shader::kGmemWindows);
    const bool verify = cs->cs_ref != nullptr;
    if (verify) {
        ctx_->CSSetShader(cs->cs_ref.Get(), nullptr, 0);
        ctx_->Dispatch(grid[0], grid[1], grid[2]);
        for (u32 i = 0; i < shader::kGmemWindows; i++) {
            if (!win_size[i])
                continue;
            GmemWindow& w = gmem_[i];
            const D3D11_BOX box{0, 0, 0, win_size[i], 1, 1};
            ctx_->CopySubresourceRegion(w.staging.Get(), 0, 0, 0, 0, w.buf.Get(), 0, &box);
            D3D11_MAPPED_SUBRESOURCE mp;
            if (SUCCEEDED(ctx_->Map(w.staging.Get(), 0, D3D11_MAP_READ, 0, &mp))) {
                reference[i].assign(static_cast<const u8*>(mp.pData), static_cast<const u8*>(mp.pData) + win_size[i]);
                ctx_->Unmap(w.staging.Get(), 0);
            }
            ctx_->UpdateSubresource(w.buf.Get(), 0, &box, w.uploaded.data(), win_size[i], win_size[i]);
        }
        ctx_->CSSetShader(cs->cs.Get(), nullptr, 0);
    }
    ctx_->Dispatch(grid[0], grid[1], grid[2]);
    ID3D11UnorderedAccessView* no_uav[shader::kGmemWindows]{};
    ctx_->CSSetUnorderedAccessViews(0, shader::kGmemWindows, no_uav, nullptr);
    ID3D11ShaderResourceView* no_srv[48]{};
    ctx_->CSSetShaderResources(0, 48, no_srv);
    dispatches_++;
    // ---- write back what the kernel changed (the guest reads results through texel buffers and the CPU)
    for (u32 i = 0; i < shader::kGmemWindows; i++) {
        if (!win_size[i])
            continue;
        GmemWindow& w = gmem_[i];
        const D3D11_BOX box{0, 0, 0, win_size[i], 1, 1};
        ctx_->CopySubresourceRegion(w.staging.Get(), 0, 0, 0, 0, w.buf.Get(), 0, &box);
        D3D11_MAPPED_SUBRESOURCE mp;
        if (FAILED(ctx_->Map(w.staging.Get(), 0, D3D11_MAP_READ, 0, &mp)))
            continue;
        const u8* now = static_cast<const u8*>(mp.pData);
        if (verify && reference[i].size() == win_size[i]) {
            u32 diff = 0, first = ~0u;
            for (u32 off = 0; off < win_size[i]; off += 4)
                if (std::memcmp(now + off, &reference[i][off], 4) != 0) {
                    diff++;
                    if (first == ~0u)
                        first = off;
                }
            static std::map<u64, u32> reported;
            if (diff && reported[key ^ i]++ < 3) {
                u32 a4 = 0, b4 = 0;
                std::memcpy(&a4, now + first, 4);
                std::memcpy(&b4, &reference[i][first], 4);
                LOG_WARN("Renderer", "DXBC verify {:#x} window {}: {} words differ (first +{:#x}: dxbc {:08x} fxc {:08x})", prog, i,
                         diff, first, a4, b4);
            } else if (!diff && reported[key ^ i] == 0) {
                reported[key ^ i] = 100;
                LOG_INFO("Renderer", "DXBC verify {:#x} window {}: identical", prog, i);
            }
        }
        // Changed runs only, so data the CPU wrote meanwhile elsewhere in the window survives.
        u32 changed = 0;
        for (u32 off = 0; off < win_size[i];) {
            if (std::memcmp(now + off, w.uploaded.data() + off, 4) == 0) {
                off += 4;
                continue;
            }
            u32 end = off + 4;
            while (end < win_size[i] && std::memcmp(now + end, w.uploaded.data() + end, 4) != 0)
                end += 4;
            g.WriteGpu(win_addr[i] + off, now + off, end - off);
            if (log && changed < 0x80) {
                u32 v[4]{};
                std::memcpy(v, now + off, std::min<u32>(16, end - off));
                LOG_INFO("Renderer", "    changed +{:#x} len {:#x}: {:08x} {:08x} {:08x} {:08x}", off, end - off, v[0], v[1], v[2], v[3]);
            }
            changed += end - off;
            textures_->Invalidate(win_addr[i] + off, end - off);
            off = end;
        }
        ctx_->Unmap(w.staging.Get(), 0);
        if (log)
            LOG_INFO("Renderer", "  gmem window {} changed {:#x} bytes", i, changed);
    }
}

void D3D11Renderer::EmulateCullingGs() {
    auto& g = G();
    const auto& m = g.M3D();
    const auto& dr = m.draw;
    const auto& cb = m.CBuf(3, 0);
    if (!cb.enabled || cb.size < 0x428 || dr.topology != 4)
        return;
    u32 c0[2], base[2];
    g.ReadGpu(cb.address + 0x410, c0, 8);
    g.ReadGpu(cb.address + 0x420, base, 8);
    const u64 counter_va = u64(c0[0]) | (u64(c0[1]) << 32), list_va = u64(base[0]) | (u64(base[1]) << 32);
    if (!counter_va || !list_va)
        return;
    std::vector<u32> ids(dr.count);
    if (dr.indexed) {
        const u32 fmt = m.Reg(kIndexFormat);
        const u32 isz = fmt == 0 ? 1 : fmt == 1 ? 2 : 4;
        std::vector<u8> raw(size_t(dr.count) * isz);
        g.ReadGpu(m.Addr(kIndexAddrHigh) + u64(dr.first) * isz, raw.data(), raw.size());
        for (u32 i = 0; i < dr.count; i++)
            ids[i] = isz == 1 ? raw[i] : isz == 2 ? common::Read<u16>(&raw[i * 2]) : common::Read<u32>(&raw[i * 4]);
    } else {
        for (u32 i = 0; i < dr.count; i++)
            ids[i] = dr.first + i;
    }
    const u32 tris = dr.count / 3;
    if (!tris)
        return;
    std::vector<u32> out(size_t(tris) * 3);
    for (u32 t = 0; t < tris; t++) {
        out[t * 3 + 0] = ids[t * 3 + 0];
        out[t * 3 + 1] = ids[t * 3 + 1];
        out[t * 3 + 2] = ids[t * 3 + 2];
    }
    u32 counter = 0;
    g.ReadGpu(counter_va, &counter, 4);
    if (trace_draws_ > 0)
        LOG_INFO("Renderer", "TRACE culling-gs: counter {:#x} = {} list {:#x} +{} tris", counter_va, counter, list_va, tris);
    g.WriteGpu(list_va + u64(counter) * 4, out.data(), out.size() * 4);
    counter += tris * 3;
    g.WriteGpu(counter_va, &counter, 4);
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
        // Shader lookups are cached by code address; GPU writes to the code region flush the cache.
        const u64 key = addr ^ (u64(stage) << 60);
        if (auto cached = shader_by_addr_.find(key); cached != shader_by_addr_.end())
            return cached->second;
        static thread_local std::vector<u8> buf;
        buf.assign(0x10000, 0);
        const size_t got = g.ReadGpuPartial(addr, buf.data(), buf.size());
        if (got < 0x60)
            return nullptr;
        HostShader* sh = shaders_->Get(stage, buf.data(), got);
        shader_by_addr_[key] = sh;
        code_lo_ = std::min(code_lo_, addr);
        code_hi_ = std::max(code_hi_, addr + shader::ProgramSize(buf.data(), got));
        return sh;
    };
    HostShader* vs = load_shader(1, shader::Stage::Vertex);
    if (!vs)
        vs = load_shader(0, shader::Stage::Vertex);
    HostShader* ps = load_shader(5, shader::Stage::Fragment);
    // A geometry program without any OUT (emit) instruction rasterizes nothing: MUA3 uses one to cull triangles
    // into a buffer ahead of its real draws. Drawing such a pass would leave depth the game never wrote.
    if (m.Reg(kShaderConfig + 4 * 0x10) & 1) {
        const u64 addr = code_base + m.Reg(kShaderConfig + 4 * 0x10 + 1);
        auto it = gs_emits_.find(addr);
        if (it == gs_emits_.end()) {
            std::vector<u8> buf(0x10000, 0);
            const size_t got = g.ReadGpuPartial(addr, buf.data(), buf.size());
            const size_t size = shader::ProgramSize(buf.data(), got);
            bool emits = false;
            for (size_t off = 0x50 + 8; off + 8 <= size; off += 8) {
                if (((off - 0x50) % 32) == 0)
                    continue; // scheduling control word
                u64 insn;
                std::memcpy(&insn, &buf[off], 8);
                const shader::Op op = shader::Decode(insn);
                emits |= op == shader::Op::OUT_reg || op == shader::Op::OUT_cbuf || op == shader::Op::OUT_imm;
            }
            it = gs_emits_.emplace(addr, emits).first;
            LOG_INFO("Renderer", "geometry program at {:#x}: {}", addr, emits ? "emits primitives (not emulated)" : "emits nothing");
        }
        if (!it->second) {
            EmulateCullingGs();
            draws_skipped++, skip_reason_[5]++;
            return;
        }
    }
    // Geometry/tessellation programs are not run yet: dump them once so they can be inspected.
    for (u32 prog = 2; prog <= 4; prog++) {
        const u32 cfg = m.Reg(kShaderConfig + prog * 0x10);
        if (!(cfg & 1))
            continue;
        static std::set<u64> dumped;
        const u64 addr = code_base + m.Reg(kShaderConfig + prog * 0x10 + 1);
        if (!dumped.insert(addr).second)
            continue;
        std::vector<u8> buf(0x10000, 0);
        const size_t got = g.ReadGpuPartial(addr, buf.data(), buf.size());
        buf.resize(shader::ProgramSize(buf.data(), got));
        common::WriteFile(fmt::format("shader_dump/{:016x}.prog{}.bin", Hash64(buf.data(), buf.size()), prog), buf);
        LOG_INFO("Renderer", "program {} enabled (not emulated), dumped {} bytes", prog, buf.size());
    }
    // Integer vertex attributes and integer textures need integer-typed shader declarations (a float4
    // declaration would receive converted values): pick the matching shader variant.
    auto int_textures = [&](HostShader* sh, u32 stage_index, ShaderCache::VariantKey& key) {
        const auto& tcb = m.CBuf(stage_index, m.Reg(kTexCbIndex));
        const u64 tic_base = m.Addr(kTicAddrHigh);
        for (auto& t : sh->info.textures) {
            if (t.slot >= 32)
                continue;
            u32 handle = 0;
            g.ReadGpu(tcb.address + t.cbuf_offset, &handle, 4);
            TicEntry tic;
            g.ReadGpu(tic_base + u64(handle & 0xFFFFF) * 32, tic.w.data(), 32);
            if (FindSurfaceByGpu(tic.Address()))
                continue; // render targets are float/unorm surfaces
            switch (DxgiIntKind(TicFormat(tic.Format(), tic.RType(), tic.Srgb()).dxgi)) {
            case 1: key.tex_uint |= 1u << t.slot; break;
            case 2: key.tex_sint |= 1u << t.slot; break;
            default: break;
            }
        }
    };
    if (vs && vs->state == HostShader::Ready) {
        ShaderCache::VariantKey key;
        for (u32 i = 0; i < 32; i++) {
            if (!(vs->info.attrib_in_mask & (1u << i)))
                continue;
            const u32 f = m.Reg(kVertexAttribFormat + i);
            const u32 b = f & 0x1F;
            if ((f & 0x40) || b >= 16 || !(m.Reg(kVertexArray + b * 4) & 0x1000) || AttribFormat(f) == DXGI_FORMAT_UNKNOWN)
                continue; // fed from the float default slot
            if (((f >> 21) & 0x3F) == 0x30 && ((f >> 27) & 7) == 1) {
                key.attr_snorm10 |= 1u << i;
                continue;
            }
            switch ((f >> 27) & 7) {
            case 3: key.attr_sint |= 1u << i; break;
            case 4: key.attr_uint |= 1u << i; break;
            case 5: key.attr_uint |= 1u << i; key.attr_scaled |= 1u << i; break;
            case 6: key.attr_sint |= 1u << i; key.attr_scaled |= 1u << i; break;
            default: break;
            }
        }
        int_textures(vs, 0, key);
        vs = shaders_->GetVariant(vs, key);
    }
    if (ps && ps->state == HostShader::Ready) {
        ShaderCache::VariantKey key;
        int_textures(ps, 4, key);
        ps = shaders_->GetVariant(ps, key);
    }
    // Debug: MUA3_SKIP_PS=<hash>,<hash>... drops every draw using one of those pixel shaders.
    static const std::string skip_ps = [] {
        const char* e = std::getenv("MUA3_SKIP_PS");
        return std::string(e ? e : "");
    }();
    if (ps && !skip_ps.empty() && skip_ps.find(fmt::format("{:016x}", ps->hash)) != std::string::npos)
        return;
    if (!vs || vs->state != HostShader::Ready || (ps && ps->state != HostShader::Ready)) {
        draws_skipped++, skip_reason_[0]++;
        static std::set<u64> pending_logged;
        for (HostShader* sh : {vs, ps})
            if (sh && sh->state == HostShader::Pending && pending_logged.size() < 64 && pending_logged.insert(sh->hash).second)
                LOG_INFO("Renderer", "draw waiting on {} {:016x}{}", sh->stage == shader::Stage::Vertex ? "vs" : "ps",
                         sh->hash, sh->is_variant ? " (integer-type variant)" : "");
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
    draw_serial_++;
    for (u32 i = 0; i < 8; i++)
        bound_[i] = i < count ? colors[i] : nullptr;
    bound_[8] = depth;

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
    // Pixel history (debug "pick x y"): restrict every draw of one frame to that pixel and count samples.
    const bool counting = occl_frame_ == frame_counter_; // debug "occl": samples written by every draw
    const bool picking = (pick_frame_ == frame_counter_ && colors[0] && colors[0]->width >= 1280 / 2) || counting;
    if (picking && !counting) {
        const u32 px = u32(pick_x_ * colors[0]->width), py = u32(pick_y_ * colors[0]->height);
        sr = {std::max<LONG>(sr.left, LONG(px)), std::max<LONG>(sr.top, LONG(py)), std::min<LONG>(sr.right, LONG(px + 1)),
              std::min<LONG>(sr.bottom, LONG(py + 1))};
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
    // Debug: MUA3_TESTS_OFF=<ps hash>:<z|s|zs> disables the depth and/or stencil test for one pixel shader.
    static const std::pair<u64, std::string> tests_off = [] {
        const char* e = std::getenv("MUA3_TESTS_OFF");
        if (!e)
            return std::pair<u64, std::string>{0, ""};
        const std::string v = e;
        const size_t c = v.find(':');
        return std::pair<u64, std::string>{std::strtoull(v.substr(0, c).c_str(), nullptr, 16),
                                           c == std::string::npos ? "zs" : v.substr(c + 1)};
    }();
    if (ps && tests_off.first == ps->hash) {
        if (tests_off.second.find('z') != std::string::npos)
            dd.DepthEnable = FALSE;
        if (tests_off.second.find('s') != std::string::npos)
            dd.StencilEnable = FALSE;
    }
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
    // Polygon offset (shadow casters): units are stored doubled, slope factor and clamp as floats.
    if (m.Reg(kPolygonOffsetFillEnable) & 1) {
        rd.DepthBias = INT(std::lround(m.RegF(kDepthBias) * 0.5f));
        rd.SlopeScaledDepthBias = m.RegF(kSlopeScaleDepthBias);
        rd.DepthBiasClamp = m.RegF(kDepthBiasClamp);
    }
    // Debug: MUA3_ZBIAS_PS=<ps hash>:<depth bias units> biases one pixel shader's depth.
    static const std::pair<u64, int> zbias = [] {
        const char* e = std::getenv("MUA3_ZBIAS_PS");
        if (!e)
            return std::pair<u64, int>{0, 0};
        const std::string v = e;
        const size_t c = v.find(':');
        return std::pair<u64, int>{std::strtoull(v.substr(0, c).c_str(), nullptr, 16),
                                   c == std::string::npos ? 0 : std::atoi(v.c_str() + c + 1)};
    }();
    if (ps && zbias.first == ps->hash)
        rd.DepthBias = zbias.second;
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
        draws_skipped++, skip_reason_[1]++;
        return;
    }
    if (indices.empty()) {
        draws_skipped++, skip_reason_[2]++;
        return;
    }
    min_v = *std::min_element(indices.begin(), indices.end());
    max_v = *std::max_element(indices.begin(), indices.end());
    if (max_v - min_v > 4000000) {
        draws_skipped++, skip_reason_[3]++;
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
                // Maxwell issues one draw per instance; the buffer is uploaded starting at this
                // instance's element, so every draw reads element 0.
                e.InputSlotClass = D3D11_INPUT_PER_INSTANCE_DATA;
                e.InstanceDataStepRate = 1;
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
            draws_skipped++, skip_reason_[4]++;
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
            begin = addr + u64(stride) * (dr.instance / divisor);
            end = std::min<u64>(limit + 1, begin + stride + 64);
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
        if (trace_draws_ > 0 && b == 0) {
            trace_vb_data_.assign(tmp.begin(), tmp.begin() + size);
            trace_vb_stride_ = stride;
        }
        if (trace_draws_ > 0 && b == 0) { // raw bytes of the first two vertices, for the draw trace
            trace_vb_ = fmt::format("b0 stride {} div {} first {} min_v {} max_v {} limit {:#x} need {:#x}:", stride, divisor,
                                    begin - addr, min_v, max_v, limit + 1 - addr, u64(max_v + 1) * stride);
            for (u32 i = 0; i < std::min<u32>(size, stride * 2); i++)
                trace_vb_ += fmt::format("{}{:02x}", i % 4 ? "" : " ", tmp[i]);
        }
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
    drv.misc[0] = dr.instance; // added to SV_InstanceID (always 0: each Maxwell draw is one instance)
    drv.misc[1] = m.Reg(kAlphaTestFunc) >= 0x200 ? m.Reg(kAlphaTestFunc) - 0x200 + 1 : m.Reg(kAlphaTestFunc);
    drv.misc[2] = m.Reg(kAlphaTestRef);
    drv.misc[3] = m.Reg(kAlphaTestEnable) & 1;

    // Trace every draw into the presented surface once (debug aid, MUA3_TRACE_DRAWS).
    // With trace_rt_ set, trace draws that write that surface (any MRT slot) instead.
    trace_draw_ = false;
    if (trace_draws_ > 0) {
        const u64 want = trace_rt_;
        if (want == ~0ull) // "all": every draw
            trace_draw_ = true;
        for (u32 i = 0; i < count && !trace_draw_; i++)
            trace_draw_ = colors[i] && (want ? colors[i]->gpu_va == want : presented_cpu_.count(colors[i]->cpu_va) != 0);
    }
    if (trace_draw_) {
        trace_draws_--;
        std::string rts;
        for (u32 i = 0; i < count; i++)
            rts += colors[i] ? fmt::format(" {:#x}/{:#x}", colors[i]->gpu_va, colors[i]->format) : " -";
        std::string attrs;
        for (u32 i = 0; i < 32; i++)
            if (vs->info.attrib_in_mask & (1u << i)) {
                const u32 f = m.Reg(kVertexAttribFormat + i);
                attrs += fmt::format(" {}:b{}+{}/s{:x}t{}{}", i, f & 0x1F, (f >> 7) & 0x3FFF, (f >> 21) & 0x3F, (f >> 27) & 7,
                                     (f & 0x40) ? "c" : "");
            }
        std::string cbs;
        for (u32 i = 0; i < 18; i++)
            if (vs->info.cbuf_mask & (1u << i))
                cbs += fmt::format(" c{}={:#x}@{:#x}", i, m.CBuf(0, i).size, m.CBuf(0, i).address);
        std::string progs;
        for (u32 pidx = 0; pidx < 6; pidx++) {
            const u32 cfg = m.Reg(kShaderConfig + pidx * 0x10);
            progs += fmt::format(" p{}={}{:x}", pidx, cfg & 1 ? "on:" : "", (cfg >> 4) & 0xF);
        }
        LOG_INFO("Renderer", "TRACE attrs{} | {} | vs cbufs{} | progs{} topo {} | base vertex {:#x} base instance {:#x} idxfmt {} first {}",
                 attrs, trace_vb_, cbs, progs, dr.topology, m.Reg(0x50D), m.Reg(0x50E), m.Reg(kIndexFormat), dr.first);
        // CPU check of the first triangle: attribute-5 positions through bone 0 (c3[14..16]) and c3[10..13].
        if ((vs->info.cbuf_mask & 8) && trace_vb_stride_ && indices.size() >= 3) {
            float c[20][4]{};
            g.ReadGpu(m.CBuf(0, 3).address, c, std::min<size_t>(sizeof(c), m.CBuf(0, 3).size));
            const u32 f5 = m.Reg(kVertexAttribFormat + 5), off5 = (f5 >> 7) & 0x3FFF;
            std::string tri;
            for (u32 k = 0; k < 3; k++) {
                const size_t at = size_t(indices[k] - min_v) * trace_vb_stride_ + off5;
                if (at + 12 > trace_vb_data_.size())
                    break;
                float p[4] = {0, 0, 0, 1};
                std::memcpy(p, &trace_vb_data_[at], 12);
                float w[4] = {0, 0, 0, 1};
                for (int r = 0; r < 3; r++)
                    w[r] = p[0] * c[14 + r][0] + p[1] * c[14 + r][1] + p[2] * c[14 + r][2] + c[14 + r][3];
                float clip[4];
                for (int col = 0; col < 4; col++)
                    clip[col] = w[0] * c[10][col] + w[1] * c[11][col] + w[2] * c[12][col] + c[13][col];
                tri += fmt::format(" v{}=({:g},{:g},{:g}) ndc=({:.3f},{:.3f},{:.4f})", indices[k], p[0], p[1], p[2],
                                   clip[0] / clip[3], clip[1] / clip[3], clip[2] / clip[3]);
            }
            u32 behind = 0, far_off = 0, total = 0;
            float min_w = 1e30f;
            for (size_t k = 0; k + 2 < indices.size(); k += 3) {
                bool b = false, f = false;
                for (int e = 0; e < 3; e++) {
                    const size_t at = size_t(indices[k + e] - min_v) * trace_vb_stride_ + off5;
                    if (at + 12 > trace_vb_data_.size())
                        continue;
                    float p[3];
                    std::memcpy(p, &trace_vb_data_[at], 12);
                    float w[3];
                    for (int r = 0; r < 3; r++)
                        w[r] = p[0] * c[14 + r][0] + p[1] * c[14 + r][1] + p[2] * c[14 + r][2] + c[14 + r][3];
                    float clip[4];
                    for (int col = 0; col < 4; col++)
                        clip[col] = w[0] * c[10][col] + w[1] * c[11][col] + w[2] * c[12][col] + c[13][col];
                    min_w = std::min(min_w, clip[3]);
                    b |= clip[3] <= 0;
                    f |= std::abs(clip[0] / clip[3]) > 4 || std::abs(clip[1] / clip[3]) > 4;
                }
                total++, behind += b, far_off += f;
            }
            LOG_INFO("Renderer", "TRACE tri{} | tris {} behind-camera {} far {} min_w {:g}", tri, total, behind, far_off, min_w);
        }
        if (ps) {
            for (u32 cb : {1u, 3u}) {
                if (!(ps->info.cbuf_mask & (1u << cb)))
                    continue;
                float rows[176][4]{};
                g.ReadGpu(m.CBuf(4, cb).address, rows, std::min<size_t>(sizeof(rows), m.CBuf(4, cb).size));
                const u32 n = std::min<u32>(176, m.CBuf(4, cb).size / 16);
                for (u32 base = 0; base < n; base += 16) {
                    std::string r;
                    for (u32 k = base; k < std::min(n, base + 16); k++)
                        r += fmt::format(" [{}]({:g},{:g},{:g},{:g})", k, rows[k][0], rows[k][1], rows[k][2], rows[k][3]);
                    LOG_INFO("Renderer", "TRACE ps c{} size {:#x}:{}", cb, m.CBuf(4, cb).size, r);
                }
            }
        }
        if (vs->info.cbuf_mask & 8) {
            float rows[20][4]{};
            g.ReadGpu(m.CBuf(0, 3).address, rows, std::min<size_t>(sizeof(rows), m.CBuf(0, 3).size));
            std::string r;
            for (auto& row : rows)
                r += fmt::format(" ({:g},{:g},{:g},{:g})", row[0], row[1], row[2], row[3]);
            LOG_INFO("Renderer", "TRACE c3[0..19]{}", r);
        }
        LOG_INFO("Renderer", "TRACE ib {:#x} first {} count {} indexed {}", m.Addr(kIndexAddrHigh), dr.first, dr.count, dr.indexed);
        LOG_INFO("Renderer", "TRACE vp sz {:g} tz {:g} sy {:g} near {:g} far {:g} depthmode {:#x} clipctl {:#x}", m.RegF(kViewportTransform + 2),
                 m.RegF(kViewportTransform + 5), m.RegF(kViewportTransform + 1), m.RegF(kViewport + 2), m.RegF(kViewport + 3),
                 m.Reg(0x35F), m.Reg(0x64F));
        LOG_INFO("Renderer", "TRACE draw vs {:016x} ps {:016x} n {} rts{} blend0 {} cmask {:#x} depth {} ztest {} zwrite {} zfunc {:#x}",
                 vs->hash, ps ? ps->hash : 0, indices.size(), rts, m.Reg(kBlendEnable), m.Reg(kColorMask),
                 depth ? fmt::format("{:#x}/{:#x}", depth->gpu_va, depth->format) : "-", m.Reg(kDepthTestEnable),
                 m.Reg(kDepthWriteEnable), m.Reg(kDepthTestFunc));
        LOG_INFO("Renderer", "TRACE stencil en {} func {:#x} ref {:#x} fmask {:#x} wmask {:#x} ops {:#x}/{:#x}/{:#x} twoside {} back ref {:#x} wmask {:#x} fmask {:#x} func {:#x} ops {:#x}/{:#x}/{:#x} cull {} {:#x} front {:#x}",
                 m.Reg(kStencilEnable), m.Reg(kStencilFrontFunc), m.Reg(kStencilFrontRef), m.Reg(kStencilFrontFuncMask),
                 m.Reg(kStencilFrontMask), m.Reg(kStencilFrontOpFail), m.Reg(kStencilFrontOpZFail), m.Reg(kStencilFrontOpZPass),
                 m.Reg(kStencilTwoSide), m.Reg(0x3D5), m.Reg(0x3D6), m.Reg(0x3D7), m.Reg(kStencilBackFunc), m.Reg(kStencilBackOpFail),
                 m.Reg(kStencilBackOpZFail), m.Reg(kStencilBackOpZPass), m.Reg(0x646), m.Reg(0x648), m.Reg(0x647));
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
            // Upload only what the shader can read: its highest static row, or everything for dynamic indexing.
            const u32 need = (s->info.cbuf_dyn_mask & (1u << i)) ? 65536u : s->info.cbuf_rows[i] * 16u;
            const u32 size = u32(common::AlignUp(std::max<u32>(std::min<u32>({b.size, 65536u, need}), 16u), 16));
            tmp.resize(size);
            g.ReadGpu(b.address, tmp.data(), size);
            cbs[i] = dc.Constant(stage_index * 32 + i, tmp.data(), size);
            if (trace_draw_ && !is_vs && size >= 13 * 16) {
                const float* f = reinterpret_cast<const float*>(tmp.data());
                LOG_INFO("Renderer", "TRACE upload ps c{} {:#x}@{:#x} size {:#x} dyn {} rows10-12.w {:g} {:g} {:g}", i, b.size, b.address, size,
                         (s->info.cbuf_dyn_mask >> i) & 1, f[10 * 4 + 3], f[11 * 4 + 3], f[12 * 4 + 3]);
            }
            if (s->info.cbuf_dyn_mask & (1u << i)) {
                ID3D11ShaderResourceView* raw = dc.RawBuffer(stage_index * 32 + i, tmp.data(), size);
                if (is_vs)
                    ctx_->VSSetShaderResources(32 + i, 1, &raw);
                else
                    ctx_->PSSetShaderResources(32 + i, 1, &raw);
            }
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
                LOG_INFO("Renderer", "    {} tex slot {} handle {:#x} tic {} addr {:#x} fmt {:#x} type {} {}x{} w0 {:#x} swz {:#x} srgb {} mips {} {}", is_vs ? "VS" : "PS",
                         t.slot, handle, tic_idx, tic.Address(), tic.Format(), tic.Type(), tic.Width(), tic.Height(), tic.w[0], swz, tic.Srgb(), tic.MaxMip(),
                         FindSurfaceByGpu(tic.Address()) ? "(render target)" : "");
            drv.swizzle[t.slot][0] = swz;
            if (t.sampler < 16)
                smps[t.sampler] = textures_->GetSampler(tsc, t.shadow);
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
    ComPtr<ID3D11Query> pick_query;
    if (picking) {
        D3D11_QUERY_DESC qd{D3D11_QUERY_OCCLUSION, 0};
        dev_->CreateQuery(&qd, &pick_query);
        ctx_->Begin(pick_query.Get());
    }
    ctx_->DrawIndexedInstanced(u32(indices.size()), 1, 0, base_vertex, 0);
    if (pick_query) {
        ctx_->End(pick_query.Get());
        UINT64 samples = 0;
        while (ctx_->GetData(pick_query.Get(), &samples, sizeof(samples), 0) == S_FALSE) {
        }
        if (samples || counting)
            LOG_INFO("Renderer", "{} draw vs {:016x} ps {:016x} rt0 {:#x} n {} blend {} zwrite {} samples {}",
                     counting ? "OCCL" : "PICK", vs->hash, ps ? ps->hash : 0, colors[0] ? colors[0]->gpu_va : 0,
                     indices.size(), m.Reg(kBlendEnable), m.Reg(kDepthWriteEnable), samples);
    }
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
    {
        std::lock_guard lk(shot_mutex_);
        if (!shot_path_.empty() && s) {
            DumpSurface(s, shot_path_);
            LOG_INFO("Renderer", "Saved frame to {}", shot_path_.string());
            shot_path_.clear();
        }
    }
    presented_cpu_.insert(p.cpu_addr);
    {
        std::vector<PixelReq> reqs;
        {
            std::lock_guard l(pixel_mutex_);
            reqs.swap(pixel_reqs_);
        }
        for (const PixelReq& r : reqs) {
            Surface* sf = FindSurfaceByGpu(r.va);
            if (!sf || r.x >= sf->width || r.y >= sf->height) {
                LOG_INFO("Renderer", "PIXEL {:#x} ({},{}): no such surface/texel", r.va, r.x, r.y);
                continue;
            }
            D3D11_TEXTURE2D_DESC td;
            sf->tex->GetDesc(&td);
            td.Width = td.Height = 1;
            td.Usage = D3D11_USAGE_STAGING;
            td.BindFlags = 0;
            td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            ComPtr<ID3D11Texture2D> st;
            if (FAILED(dev_->CreateTexture2D(&td, nullptr, &st)))
                continue;
            if (sf->depth) // depth-stencil copies must be whole-resource; read the full copy instead
                continue;
            const D3D11_BOX box{r.x, r.y, 0, r.x + 1, r.y + 1, 1};
            ctx_->CopySubresourceRegion(st.Get(), 0, 0, 0, 0, sf->tex.Get(), 0, &box);
            D3D11_MAPPED_SUBRESOURCE mp;
            if (SUCCEEDED(ctx_->Map(st.Get(), 0, D3D11_MAP_READ, 0, &mp))) {
                u32 words[4]{};
                std::memcpy(words, mp.pData, 16);
                ctx_->Unmap(st.Get(), 0);
                LOG_INFO("Renderer", "PIXEL {:#x} fmt {:#x} ({},{}): {:08x} {:08x} {:08x} {:08x}", r.va, sf->format, r.x, r.y,
                         words[0], words[1], words[2], words[3]);
            }
        }
    }
    if (dump_surfaces_.exchange(false)) {
        int n = 0;
        const std::string dir = fmt::format("screenshots/rt/{}", frame_counter_);
        std::error_code ec;
        fs::create_directories(dir, ec);
        LOG_INFO("Renderer", "dumping {} surfaces to {}", surfaces_.size(), dir);
        for (auto& [k, sf] : surfaces_)
            if (!sf->depth)
                DumpSurface(sf.get(), fmt::format("{}/surf_{:02}_{:x}_{}x{}_{:x}{}.bmp", dir, n++, sf->gpu_va,
                                                  sf->width, sf->height, sf->format, sf.get() == s ? "_present" : ""));
    }
    if (frame_counter_ % 150 == 1) {
        LOG_INFO("Renderer", "Present cpu {:#x} {}x{}: surface {} | draws ok {} skipped {} [shader {} topo {} empty {} range {} layout {} gs-no-output {}] clears {} surfaces {} shaders {} (failed {})",
                 p.cpu_addr, p.width, p.height, s ? fmt::format("{:#x}", s->gpu_va) : "none", draws_ok, draws_skipped,
                 skip_reason_[0], skip_reason_[1], skip_reason_[2], skip_reason_[3], skip_reason_[4], skip_reason_[5],
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
    if (recent_.size() > 256)
        recent_.pop_back();
}

ID3D11ShaderResourceView* D3D11Renderer::LatestFrame(u32& w, u32& h) {
    w = frame_w_;
    h = frame_h_;
    return frame_srv_.Get();
}

void D3D11Renderer::InvalidateRegion(u64 gpu_va, u64 size) {
    textures_->Invalidate(gpu_va, size);
    if (gpu_va < code_hi_ && code_lo_ < gpu_va + size) {
        shader_by_addr_.clear();
        compute_by_key_.clear();
        code_lo_ = ~0ull;
        code_hi_ = 0;
    }
}

bool D3D11Renderer::Blit2D(const u32* regs) { return false; }

void D3D11Renderer::FlushCommands() {}

void D3D11Renderer::BindRenderTargets(Surface**, u32, Surface*) {}

} // namespace gpu
