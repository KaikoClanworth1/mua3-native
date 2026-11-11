#include "gpu/d3d11/renderer_d3d11.h"
#include "core/hle/nvn/nvn_hle.h"
#include "gpu/shader/maxwell_isa.h"

#include <chrono>
#include <cstdio>
#include <sstream>
#include <thread>
#include <emmintrin.h>
#include <unordered_set>
#include <set>

#include <algorithm>

#include <d3d11sdklayers.h>
#include <dxgi1_4.h>
#include <d3dcompiler.h>

#include "core/memory.h"
#include "core/system.h"
#include "gpu/d3d11/shader_cache.h"
#include "gpu/d3d11/texture_cache.h"
#include "gpu/formats.h"
#include "gpu/gpu.h"

namespace gpu {


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

// MUA3_PROFILE: time spent per Draw() section and bytes uploaded, logged every few seconds at Present.
static const bool g_profile = std::getenv("MUA3_PROFILE") != nullptr;
enum ProfSection { kProfShaders, kProfState, kProfIndex, kProfVertex, kProfCbufs, kProfTextures, kProfResources, kProfSubmit, kProfCount };
static const char* const kProfNames[kProfCount] = {"shaders", "state", "index", "vertex", "cbufs", "textures", "other", "submit"};
static u64 g_prof_ns[kProfCount], g_prof_vb_bytes, g_prof_cb_bytes, g_prof_ib_count, g_prof_draws;
static u64 g_prof_rb_waits, g_prof_rb_wait_ns, g_prof_rb_queued, g_prof_rb_bytes; // compute result copy-backs
static u64 g_prof_resident; // draws reading compute output from the resident copy
static u64 g_prof_rb_pages, g_prof_rb_changed_pages; // copy-back pages compared / found changed
static u64 g_prof_rb_not_ready; // polls that found the oldest copy-back still in flight
static u64 g_prof_track_bytes[3]; // bytes write-protected again per consumer: 0 textures, 1 compute, 2 index/vertex
static u64 g_prof_rb_site[12]; // waits by consumer: 0 compute, 1 texture, 2/3 culling-gs, 4 index, 5 vertex, 6 cbuf, 7 geometry
static u64 g_prof_tex_get_ns, g_prof_tex_samp_ns, g_prof_tex_calls;
static u64 g_prof_last = 0;
static int g_prof_section = -1;
static u64 ProfNow() {
    LARGE_INTEGER c, f;
    QueryPerformanceCounter(&c);
    static const u64 freq = (QueryPerformanceFrequency(&f), u64(f.QuadPart));
    return u64(c.QuadPart) * 1000000000ull / freq;
}
static void Prof(int section) {
    if (!g_profile)
        return;
    const u64 now = ProfNow();
    if (g_prof_section >= 0)
        g_prof_ns[g_prof_section] += now - g_prof_last;
    g_prof_section = section;
    g_prof_last = now;
}

// Debug switches that change depth state per pixel shader: those draws can't reuse the last state object.
static const bool g_debug_ps_state = std::getenv("MUA3_TESTS_OFF") || std::getenv("MUA3_ZBIAS_PS");

// Direct3D state built from the DrawState, kept until its serial changes.
struct SeenState {
    u32 blend = 0, depth = 0, raster = 0;
    bool depth_on = false;
    float flip = 0;
    ID3D11BlendState* blend_obj = nullptr;
    ID3D11DepthStencilState* depth_obj = nullptr;
    ID3D11RasterizerState* raster_obj = nullptr;
    u64 attr_vs = 0;
    u32 attr_serial = 0;
    ShaderCache::VariantKey attr_key;
    u64 layout_vs = 0;
    u32 layout_serial = 0;
    ID3D11InputLayout* layout = nullptr;
    bool used_buf[16]{};
    bool need_default = false;
};
static SeenState g_seen;

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
    common::U64Map<ComPtr<ID3D11InputLayout>> layouts;
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
    Obj* Cached(common::U64Map<ComPtr<Obj>>& map, const Desc& d, Create create) {
        const u64 k = Hash64(&d, sizeof(d));
        auto it = map.find(k);
        if (it != map.end())
            return it->second.Get();
        ComPtr<Obj> o;
        create(&d, &o);
        if (o) // (a failed creation is retried next time rather than remembered)
            map[k] = o;
        return o.Get();
    }
    common::U64Map<ComPtr<ID3D11BlendState>> blend;
    common::U64Map<ComPtr<ID3D11DepthStencilState>> depth;
    common::U64Map<ComPtr<ID3D11RasterizerState>> raster;

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

std::atomic<u64>& ContextEpoch() {
    static std::atomic<u64> e{0};
    return e;
}

namespace {
// Sets only the changed span of a binding array (cache `cur` mirrors what the context has).
template <typename T, typename Set>
void BindRange(T* cur, const T* want_begin, u32 base, u32 n, Set set) {
    u32 lo = n, hi = 0;
    for (u32 i = 0; i < n; i++)
        if (cur[base + i] != want_begin[i]) {
            lo = std::min(lo, i);
            hi = i + 1;
        }
    if (lo >= hi)
        return;
    set(base + lo, hi - lo, want_begin + lo);
    for (u32 i = lo; i < hi; i++)
        cur[base + i] = want_begin[i];
}
} // namespace

D3D11Renderer::D3D11Renderer(ID3D11Device* dev, ID3D11DeviceContext* ctx) : dev_(dev), ctx_(ctx) {
    new_frame_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    shaders_ = std::make_unique<ShaderCache>(dev);
    shaders_->Prewarm();
    textures_ = std::make_unique<TextureCache>(dev, ctx, *this);
    g_dc = std::make_unique<DrawContext>(dev);
    g_dc->SetContext(ctx);
    if (const char* t = std::getenv("MUA3_TRACE_DRAWS"))
        trace_draws_ = std::atoi(t);
    // Test aid: MUA3_AA_AT=<seconds>=<mode>,... changes anti_aliasing while the game runs (as the Graphics menu does).
    if (const char* e = std::getenv("MUA3_AA_AT"))
        std::thread([plan = std::string(e)] {
            const auto t0 = std::chrono::steady_clock::now();
            std::istringstream in(plan);
            for (std::string item; std::getline(in, item, ',');) {
                int at = 0, mode = 0;
                if (std::sscanf(item.c_str(), "%d=%d", &at, &mode) != 2)
                    continue;
                std::this_thread::sleep_until(t0 + std::chrono::seconds(at));
                LOG_INFO("Renderer", "test: anti_aliasing -> {}", mode);
                core::Sys().UpdateConfig([mode](core::Config& c, bool) { c.anti_aliasing = mode; });
            }
        }).detach();
    if (const char* d = std::getenv("MUA3_DEBUG_PS")) {
        debug_mode_ = std::atoi(d);
        const char* src = "float4 main() : SV_Target0 { return float4(1, 0, 1, 1); }";
        ComPtr<ID3DBlob> code, err;
        if (SUCCEEDED(D3DCompile(src, std::strlen(src), nullptr, nullptr, nullptr, "main", "ps_5_0", 0, 0, &code, &err)))
            dev_->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &debug_ps_);
    }
}

D3D11Renderer::~D3D11Renderer() {
    if (new_frame_event_)
        CloseHandle(static_cast<HANDLE>(new_frame_event_));
}

u32 D3D11Renderer::TextureScale(u64 gpu_va, bool tex2d) {
    if (!tex2d)
        return 1;
    const Surface* s = FindSurfaceByGpu(gpu_va);
    return s && s->srv ? s->scale : 1;
}

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


namespace {
// MUA3_NVN_LEARN=1: log how the emulated path resolves surfaces, texture handles and constant buffers by GPU
// address, so the NVN driver's conventions can be learned by joining with the NVN model (docs/native_renderer_plan.md).
bool LearnEnabled() { // MUA3_NVN_LEARN=1: the ~60 ms after MUA3_NVN_DUMP wrote a frame (that frame's GPU work)
    static const bool on = std::getenv("MUA3_NVN_LEARN") != nullptr;
    return on && std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::steady_clock::now().time_since_epoch())
                         .count() < hle::nvn::g_learn_until_ms;
}
bool LearnOnce(u64 key) {
    static std::mutex m;
    static std::unordered_set<u64> seen;
    std::lock_guard lk(m);
    return seen.size() < 4000 && seen.insert(key).second;
}
} // namespace

// Supersampling (anti_aliasing 2-4: SSAA 2x/3x/4x per axis) multiplies the render scale; the frame shown is resolved
// back to the render scale (PostAA::Downsample). The product is capped (kMaxScale: VRAM), the factor dropped first.
static u32 SsaaFactor(int anti_aliasing) { return anti_aliasing >= 2 && anti_aliasing <= 4 ? u32(anti_aliasing) : 1; }
constexpr u32 kMaxScale = 4;

void D3D11Renderer::SetResolutionScale(u32 s) {
    base_scale_ = std::max<u32>(s, 1);
    applied_aa_ = core::Sys().Cfg().anti_aliasing;
    res_scale_ = ScaleFor(applied_aa_);
}

u32 D3D11Renderer::ScaleFor(int anti_aliasing) const {
    u32 f = SsaaFactor(anti_aliasing);
    while (f > 1 && base_scale_ * f > kMaxScale)
        f--;
    return base_scale_ * f;
}

// The anti-aliasing setting changed (read at every present): a new supersampling factor drops the screen-sized
// targets, which the next frame creates again at the new scale (their contents start over from guest memory).
void D3D11Renderer::ApplyAntiAliasing(int anti_aliasing) {
    applied_aa_ = anti_aliasing;
    const u32 scale = ScaleFor(anti_aliasing);
    if (scale == res_scale_)
        return;
    const size_t before = surfaces_.size();
    std::erase_if(surfaces_, [](const auto& kv) { return kv.second->screen_like; });
    LOG_INFO("Renderer", "anti-aliasing {}: internal scale {}x -> {}x ({} screen-sized targets dropped)", anti_aliasing,
             res_scale_, scale, before - surfaces_.size());
    res_scale_ = scale;
    std::fill(std::begin(bound_), std::end(bound_), nullptr);
    ContextEpoch()++;
}

Surface* D3D11Renderer::GetSurface(u64 gpu_va, u32 width, u32 height, u32 format, bool depth, u32 tile_mode) {
    if (!gpu_va || !width || !height || width > 16384 || height > 16384)
        return nullptr;
    if (LearnEnabled() && LearnOnce(gpu_va ^ (u64(format) << 48)))
        LOG_INFO("Learn", "surface {:#x} {}x{} format {:#x} depth {}", gpu_va, width, height, format, depth);
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
    // Resolution scaling applies to screen-shaped targets (the frame and its half/quarter-size passes); shadow
    // maps, lookup tables and other square or tiny targets keep their size.
    const bool screen_like = width >= 240 && height >= 135 && width > height &&
                             std::abs(int(width * 9) - int(height * 16)) <= int(width * 9 / 16);
    s->scale = screen_like ? res_scale_ : 1;
    s->screen_like = screen_like;
    if (screen_like) { // (the game's dynamic resolution shows up as new frame-sized targets)
        static std::set<u64> sizes;
        if (sizes.insert((u64(width) << 32) | height).second)
            LOG_INFO("Renderer", "screen-sized target {}x{} (format {:#x}{}) first seen", width, height, format, depth ? ", depth" : "");
    }
    const u32 tw = width * s->scale, th = height * s->scale;
    s->format = format;
    s->depth = depth;
    s->dxgi = fi.dxgi;
    s->last_use = frame_counter_;
    D3D11_TEXTURE2D_DESC td{tw, th, 1, 1, Typeless(fi.dxgi), {1, 0}, D3D11_USAGE_DEFAULT,
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
            if (s->scale > 1) { // nearest-neighbour upscale of the initial contents
                std::vector<u8> up(size_t(tw) * th * fi.bpp);
                for (u32 y = 0; y < th; y++)
                    for (u32 x = 0; x < tw; x++)
                        std::memcpy(&up[(size_t(y) * tw + x) * fi.bpp],
                                    &initial[(size_t(y / s->scale) * width + x / s->scale) * fi.bpp], fi.bpp);
                initial.swap(up);
            }
            init.pSysMem = initial.data();
            init.SysMemPitch = tw * fi.bpp;
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
    if (s->copy_serial != s->write_serial) { // only when written since the last copy
        ctx_->CopyResource(s->copy_tex.Get(), s->tex.Get());
        s->copy_serial = s->write_serial;
    }
    return s->copy_srv.Get();
}

Surface* D3D11Renderer::GetColorSurface(u32 i) {
    if (i >= 8)
        return nullptr;
    const auto& t = G().State().color[i];
    if (t.format == 0)
        return nullptr;
    return GetSurface(t.addr, t.width, t.height, t.format, false, t.tile);
}

Surface* D3D11Renderer::GetDepthSurface() {
    const auto& d = G().State().depth;
    if (!d.enabled)
        return nullptr;
    return GetSurface(d.addr, d.width, d.height, d.format, true);
}

void D3D11Renderer::ClearColor(u32 rt, const float color[4]) {
    std::lock_guard ctx_lock(ContextMutex());
    clears++;
    if (Surface* s = GetColorSurface(rt)) {
        ctx_->ClearRenderTargetView(s->rtv.Get(), color);
        s->write_serial = ++surface_writes_;
    }
}

void D3D11Renderer::ClearDepthStencil(bool depth, bool stencil, float depth_value, u8 stencil_value) {
    std::lock_guard ctx_lock(ContextMutex());
    clears++;
    if (Surface* s = GetDepthSurface()) {
        const UINT flags = (depth ? D3D11_CLEAR_DEPTH : 0) | (stencil ? D3D11_CLEAR_STENCIL : 0);
        ctx_->ClearDepthStencilView(s->dsv.Get(), flags, depth_value, stencil_value);
        s->write_serial = ++surface_writes_;
    }
}

// Pages of a buffer the CPU wrote since they were last tracked on `channel`, as (buffer offset, size) runs.
template <class Ranges>
static void CollectCpuWrites(const Ranges& ranges, std::vector<std::pair<u32, u32>>& out, u32 channel = 1) {
    auto& mem = G().Mem();
    for (auto& r : ranges) {
        if (!mem.ConsumeDirty(r.cpu, r.size, channel))
            continue;
        for (u64 p = r.cpu & ~0xFFFull; p < r.cpu + r.size; p += 0x1000) {
            const u64 lo = std::max(p, r.cpu), hi = std::min(p + 0x1000, r.cpu + r.size);
            if (mem.ConsumeDirty(lo, hi - lo, channel))
                out.push_back({r.off + u32(lo - r.cpu), u32(hi - lo)});
        }
    }
}

void D3D11Renderer::Reupload(GpuBuf& b, bool all, u32 from) {
    auto& g = G();
    std::vector<std::pair<u32, u32>> runs;
    if (all) {
        b.snapshot.resize(b.size, 0);
        if (!from)
            b.cpu_ranges.clear();
        for (u64 off = from; off < b.size;) {
            u64 run = 0;
            const u64 cpu = g.As().TranslateRun(b.addr + off, run);
            if (!cpu || !run)
                break;
            const u64 n = std::min<u64>(run, b.size - off);
            b.cpu_ranges.push_back({u32(off), cpu, n});
            off += n;
        }
        runs.push_back({from, b.size - from});
        if (!from)
            b.cpu_pages.clear();
        b.page_serial.resize((b.size + 0xFFF) >> 12, 0);
    } else {
        runs = std::move(b.cpu_pages);
        b.cpu_pages.clear();
        CollectCpuWrites(b.cpu_ranges, runs);
        if (runs.empty())
            return;
        std::sort(runs.begin(), runs.end());
        size_t n = 0;
        for (auto& r : runs) {
            if (n && runs[n - 1].first + runs[n - 1].second >= r.first)
                runs[n - 1].second = std::max(runs[n - 1].second, r.first + r.second - runs[n - 1].first);
            else
                runs[n++] = r;
        }
        runs.resize(n);
    }
    // Readbacks issued before this upload must not write their (older) GPU data over these pages.
    b.serial++;
    for (auto [off, n] : runs) {
        n = std::min(n, b.size - off);
        g.ReadGpuPartial(b.addr + off, b.snapshot.data() + off, n);
        const D3D11_BOX box{off, 0, 0, off + n, 1, 1};
        ctx_->UpdateSubresource(b.buf.Get(), 0, &box, b.snapshot.data() + off, n, n);
        if (b.prev && off + n <= b.prev_capacity) // (prev follows guest memory)
            ctx_->UpdateSubresource(b.prev.Get(), 0, &box, b.snapshot.data() + off, n, n);
        else
            b.diff_full = true;
        for (u32 p = off >> 12; p <= (off + n - 1) >> 12 && p < b.page_serial.size(); p++)
            b.page_serial[p] = b.serial;
    }
    for (auto& r : b.cpu_ranges)
        g.Mem().TrackWrites(r.cpu, r.size, 1), g_prof_track_bytes[1] += r.size;
    b.stale = false;
}

void D3D11Renderer::FlushGpuBuf(GpuBuf& b) {
    if (b.gpu_dirty)
        QueueReadback(b);
    if (b.in_flight) { // blocks until the GPU has run everything queued before the copy
        const u64 t0 = g_profile ? ProfNow() : 0;
        if (g_profile && sync_site_ == 1) { // which textures make the GPU thread wait (first few per buffer)
            static common::U64Map<int> logged;
            if (logged[b.addr]++ < 3)
                LOG_INFO("Profile", "texture lookup waits on compute buffer {:#x}+{:#x} (texture at {:#x})", b.addr, b.size,
                         sync_va_);
        }
        PollReadbacks(true, &b);
        if (g_profile)
            g_prof_rb_waits++, g_prof_rb_wait_ns += ProfNow() - t0, g_prof_rb_site[sync_site_]++;
    }
}

D3D11Renderer::GpuBuf* D3D11Renderer::ComputeBuffer(u64 addr, u32 size) {
    GpuBuf* e = nullptr;
    for (auto& b : gbufs_)
        if (b->addr == addr && !b->retired)
            e = b.get();
    // A window inside a larger resident buffer is served by that buffer (the caller binds it with the buffer's own
    // base and size, and the kernel's address lookup lands in the right place). Overlapping windows used to land each
    // other's results on the CPU at every switch: tens of blocking copy-backs per frame in effect-heavy fights.
    if (!e || size > e->size) {
        GpuBuf* best = nullptr;
        for (auto& b : gbufs_)
            if (!b->retired && b->addr <= addr && addr + size <= b->addr + b->size && (!best || b->size > best->size))
                best = b.get();
        if (best) {
            e = best;
            addr = e->addr;
            size = e->size;
        }
    }
    // Another resident buffer overlapping this one: land its results and re-upload both from guest memory. One that
    // lies entirely inside this range is retired: from now on its windows are served by this buffer.
    for (auto& b : gbufs_) {
        if (b->retired || b.get() == e || b->addr >= addr + size || addr >= b->addr + b->size)
            continue;
        if (addr <= b->addr && b->addr + b->size <= addr + size)
            b->retired = true;
        static bool warned = false;
        if (!warned) {
            warned = true;
            LOG_WARN("Renderer", "compute buffers overlap: {:#x}+{:#x} and {:#x}+{:#x}", addr, size, b->addr, b->size);
        }
        sync_site_ = 8;
        FlushGpuBuf(*b);
        sync_site_ = 0;
        b->stale = true;
        if (e)
            e->stale = true;
    }
    if (e && size > e->capacity) { // grow on the GPU: the old contents move by a copy, nothing waits
        const u32 capacity = u32(common::AlignUp(size, 1u << 16));
        D3D11_BUFFER_DESC bd{capacity, D3D11_USAGE_DEFAULT,
                             D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_VERTEX_BUFFER | D3D11_BIND_INDEX_BUFFER,
                             0, D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS | D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS, 0};
        ComPtr<ID3D11Buffer> nb;
        ComPtr<ID3D11UnorderedAccessView> uav;
        D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
        ud.Format = DXGI_FORMAT_R32_TYPELESS;
        ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        ud.Buffer = {0, capacity / 4, D3D11_BUFFER_UAV_FLAG_RAW};
        if (SUCCEEDED(dev_->CreateBuffer(&bd, nullptr, &nb)) && SUCCEEDED(dev_->CreateUnorderedAccessView(nb.Get(), &ud, &uav))) {
            const D3D11_BOX box{0, 0, 0, e->size, 1, 1};
            ctx_->CopySubresourceRegion(nb.Get(), 0, 0, 0, 0, e->buf.Get(), 0, &box);
            e->buf = nb;
            e->uav = uav;
            e->capacity = capacity;
            e->srvs.clear();
        } else {
            sync_site_ = 10;
            FlushGpuBuf(*e);
            sync_site_ = 0;
            std::erase_if(gbufs_, [&](auto& b) { return b.get() == e; });
            active_stale_ = true;
            e = nullptr;
        }
    }
    if (!e) {
        auto nb = std::make_unique<GpuBuf>();
        nb->addr = addr;
        nb->capacity = u32(common::AlignUp(size, 1u << 16));
        // also a vertex/index buffer: draws read compute results (skinning, particles) straight from here
        D3D11_BUFFER_DESC bd{nb->capacity, D3D11_USAGE_DEFAULT,
                             D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_VERTEX_BUFFER | D3D11_BIND_INDEX_BUFFER,
                             0, D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS | D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS, 0};
        if (FAILED(dev_->CreateBuffer(&bd, nullptr, &nb->buf)))
            return nullptr;
        D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
        ud.Format = DXGI_FORMAT_R32_TYPELESS;
        ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        ud.Buffer = {0, nb->capacity / 4, D3D11_BUFFER_UAV_FLAG_RAW};
        dev_->CreateUnorderedAccessView(nb->buf.Get(), &ud, &nb->uav);
        nb->stale = true;
        e = nb.get();
        gbufs_.push_back(std::move(nb));
        // (not active until a kernel writes it)
    }
    e->last_use = frame_counter_;
    if (size > e->size) {
        // Bound larger than before: only the new tail comes from guest memory (the rest holds the kernels'
        // results; re-uploading it all would first have to wait for them to copy back).
        const u32 old = e->size;
        e->size = size;
        if (!e->stale)
            Reupload(*e, true, old);
    }
    if (e->stale) { // full re-upload: the GPU's results have to reach guest memory first
        sync_site_ = 9;
        FlushGpuBuf(*e);
        sync_site_ = 0;
        Reupload(*e, true);
        return e;
    }
    // CPU writes: upload just those pages (results for other pages stay on the GPU; see ApplyReadback).
    bool cpu_wrote = !e->cpu_pages.empty();
    for (auto& r : e->cpu_ranges)
        cpu_wrote = cpu_wrote || G().Mem().ConsumeDirty(r.cpu, r.size, 1);
    if (cpu_wrote)
        Reupload(*e, false);
    return e;
}

// Copy-backs compare on the CPU only the pages a kernel pass flagged as changed since the last copy-back (ST010
// copies back ~12 MB a frame of which ~20 KB change; comparing all of it with the snapshot was ~8% of the GPU
// thread). MUA3_RB_GPUDIFF=0 compares everything as before.
static const bool g_rb_gpudiff = [] {
    const char* e = std::getenv("MUA3_RB_GPUDIFF");
    return !(e && *e == '0');
}();

ComPtr<ID3D11Buffer> D3D11Renderer::DiffPages(GpuBuf& b, u32& pages) {
    pages = (b.size + 0xFFF) >> 12;
    if (!g_rb_gpudiff || b.size < (64u << 10))
        return nullptr;
    auto raw_srv = [&](ID3D11Buffer* buf, u32 bytes, ComPtr<ID3D11ShaderResourceView>& out) {
        D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.Format = DXGI_FORMAT_R32_TYPELESS;
        sv.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
        sv.BufferEx = {0, bytes / 4, D3D11_BUFFEREX_SRV_FLAG_RAW};
        out.Reset();
        return SUCCEEDED(dev_->CreateShaderResourceView(buf, &sv, &out));
    };
    if (!diff_cs_) {
        // One group per page: 256 threads compare 16 bytes each.
        static const char* src =
            "ByteAddressBuffer a : register(t0); ByteAddressBuffer b : register(t1); RWByteAddressBuffer m : register(u0);"
            "cbuffer c : register(b0) { uint pages; uint per_row; uint size; uint pad; };"
            "groupshared uint changed;"
            "[numthreads(256,1,1)] void main(uint3 g : SV_GroupID, uint t : SV_GroupIndex) {"
            "  uint page = g.y * per_row + g.x; if (t == 0) changed = 0; GroupMemoryBarrierWithGroupSync();"
            "  uint off = page * 4096 + t * 16;"
            "  if (page < pages && off < size) { uint4 x = a.Load4(off), y = b.Load4(off); if (any(x != y)) InterlockedOr(changed, 1); }"
            "  GroupMemoryBarrierWithGroupSync(); if (t == 0 && page < pages) m.Store(page * 4, changed); }";
        ComPtr<ID3DBlob> code, err;
        if (FAILED(D3DCompile(src, std::strlen(src), nullptr, nullptr, nullptr, "main", "cs_5_0", 0, 0, &code, &err)) ||
            FAILED(dev_->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &diff_cs_))) {
            static bool warned = false;
            if (!warned)
                LOG_WARN("Renderer", "copy-back diff shader failed: {}", err ? static_cast<const char*>(err->GetBufferPointer()) : "");
            warned = true;
            return nullptr;
        }
        D3D11_BUFFER_DESC cd{16, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0};
        if (FAILED(dev_->CreateBuffer(&cd, nullptr, &diff_cb_))) {
            diff_cs_.Reset();
            return nullptr;
        }
    }
    // prev: same capacity as the buffer (a new or grown buffer starts over with a full compare)
    if (!b.prev || b.prev_capacity != b.capacity) {
        D3D11_BUFFER_DESC bd{b.capacity, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0,
                             D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS, 0};
        b.prev.Reset();
        b.prev_capacity = 0;
        if (FAILED(dev_->CreateBuffer(&bd, nullptr, &b.prev)) || !raw_srv(b.prev.Get(), b.capacity, b.prev_srv)) {
            b.prev.Reset();
            return nullptr;
        }
        b.prev_capacity = b.capacity;
        b.diff_full = true;
    }
    if (b.buf_srv_of != b.buf.Get()) { // (the buffer object changes when it grows)
        if (!raw_srv(b.buf.Get(), b.capacity, b.buf_srv))
            return nullptr;
        b.buf_srv_of = b.buf.Get();
    }
    ComPtr<ID3D11Buffer> staging;
    if (!b.diff_full) {
        if (diff_mask_pages_ < pages) {
            const u32 want = std::max<u32>(pages, 4096);
            D3D11_BUFFER_DESC md{want * 4, D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS, 0,
                                 D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS, 0};
            diff_mask_.Reset();
            diff_mask_uav_.Reset();
            diff_mask_pages_ = 0;
            D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
            ud.Format = DXGI_FORMAT_R32_TYPELESS;
            ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
            ud.Buffer = {0, want, D3D11_BUFFER_UAV_FLAG_RAW};
            if (FAILED(dev_->CreateBuffer(&md, nullptr, &diff_mask_)) ||
                FAILED(dev_->CreateUnorderedAccessView(diff_mask_.Get(), &ud, &diff_mask_uav_))) {
                b.diff_full = true; // (prev was not brought up to date)
                return nullptr;
            }
            diff_mask_pages_ = want;
        }
        for (size_t k = 0; k < mask_pool_.size(); k++) {
            D3D11_BUFFER_DESC d;
            mask_pool_[k]->GetDesc(&d);
            if (d.ByteWidth >= pages * 4) {
                staging = std::move(mask_pool_[k]);
                mask_pool_.erase(mask_pool_.begin() + k);
                break;
            }
        }
        if (!staging) {
            D3D11_BUFFER_DESC sd{std::max<u32>(pages, 1024) * 4, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0, 0};
            if (FAILED(dev_->CreateBuffer(&sd, nullptr, &staging)))
                staging.Reset();
        }
        if (staging) {
            const u32 per_row = std::min<u32>(pages, 65535), rows = (pages + per_row - 1) / per_row;
            D3D11_MAPPED_SUBRESOURCE mp;
            if (SUCCEEDED(ctx_->Map(diff_cb_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mp))) {
                const u32 c[4] = {pages, per_row, b.size, 0};
                std::memcpy(mp.pData, c, sizeof(c));
                ctx_->Unmap(diff_cb_.Get(), 0);
            }
            ID3D11UnorderedAccessView* no_uav[shader::kGmemWindows]{};
            ctx_->CSSetUnorderedAccessViews(0, shader::kGmemWindows, no_uav, nullptr);
            ID3D11ShaderResourceView* srvs[2] = {b.buf_srv.Get(), b.prev_srv.Get()};
            ctx_->CSSetShader(diff_cs_.Get(), nullptr, 0);
            ctx_->CSSetShaderResources(0, 2, srvs);
            ctx_->CSSetUnorderedAccessViews(0, 1, diff_mask_uav_.GetAddressOf(), nullptr);
            ctx_->CSSetConstantBuffers(0, 1, diff_cb_.GetAddressOf());
            ctx_->Dispatch(per_row, rows, 1);
            ID3D11ShaderResourceView* no_srv[2]{};
            ctx_->CSSetShaderResources(0, 2, no_srv);
            ctx_->CSSetUnorderedAccessViews(0, 1, no_uav, nullptr);
            const D3D11_BOX mbox{0, 0, 0, pages * 4, 1, 1};
            ctx_->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, diff_mask_.Get(), 0, &mbox);
        }
    }
    // prev = what guest memory holds once this copy-back is applied (a copy-back without flags compares everything,
    // so prev is in step again after it)
    const D3D11_BOX box{0, 0, 0, b.size, 1, 1};
    ctx_->CopySubresourceRegion(b.prev.Get(), 0, 0, 0, 0, b.buf.Get(), 0, &box);
    b.diff_full = false;
    return staging;
}

void D3D11Renderer::QueueReadback(GpuBuf& b) {
    Readback rb{&b, nullptr, b.size, b.serial};
    for (size_t k = 0; k < staging_pool_.size(); k++) {
        D3D11_BUFFER_DESC d;
        staging_pool_[k]->GetDesc(&d);
        if (d.ByteWidth >= b.size) {
            rb.staging = std::move(staging_pool_[k]);
            staging_pool_.erase(staging_pool_.begin() + k);
            break;
        }
    }
    if (!rb.staging) {
        D3D11_BUFFER_DESC sd{b.capacity, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0, 0};
        if (FAILED(dev_->CreateBuffer(&sd, nullptr, &rb.staging)))
            return;
    }
    // (the page flags are copied first: when the data copy has landed, they have too)
    rb.mask = DiffPages(b, rb.pages);
    const D3D11_BOX box{0, 0, 0, b.size, 1, 1};
    ctx_->CopySubresourceRegion(rb.staging.Get(), 0, 0, 0, 0, b.buf.Get(), 0, &box);
    if (!query_pool_.empty()) {
        rb.done = std::move(query_pool_.back());
        query_pool_.pop_back();
    } else {
        const D3D11_QUERY_DESC qd{D3D11_QUERY_EVENT, 0};
        dev_->CreateQuery(&qd, &rb.done);
    }
    if (rb.done)
        ctx_->End(rb.done.Get());
    b.in_flight++;
    b.gpu_dirty = false;
    MarkActive(b);
    g_prof_rb_queued++, g_prof_rb_bytes += b.size;
    readbacks_.push_back(std::move(rb));
}

void D3D11Renderer::ApplyReadback(GpuBuf& b, const u8* now, u32 size, u32 serial, const u32* mask, u32 pages) {
    auto& g = G();
    size = std::min<u32>(size, u32(b.snapshot.size()));
    if (b.apply_full)
        mask = nullptr;
    // CPU writes made meanwhile survive (only words the kernels changed are written); those pages re-upload later.
    CollectCpuWrites(b.cpu_ranges, b.cpu_pages);
    u8* old = b.snapshot.data();
    u32 lo = ~0u, hi = 0;
    // Runs of changed 32-bit words are written back; words are compared four at a time (SSE2), and unchanged pages
    // are skipped with one memcmp. (A memcmp call per word cost the GPU thread ~10% in fights.)
    u32 run = ~0u;
    auto flush = [&](u32 end) {
        g.WriteGpu(b.addr + run, now + run, end - run);
        std::memcpy(old + run, now + run, end - run);
        lo = std::min(lo, run);
        hi = std::max(hi, end);
        run = ~0u;
    };
    auto word = [&](u32 off, bool same) {
        if (!same) {
            if (run == ~0u)
                run = off;
        } else if (run != ~0u) {
            flush(off);
        }
    };
    for (u32 page = 0; page * 0x1000u < size; page++) {
        const u32 pbeg = page * 0x1000u, pend = std::min(size, pbeg + 0x1000u);
        if (page < b.page_serial.size() && b.page_serial[page] > serial)
            continue; // re-uploaded from guest memory after this copy was taken: guest data is newer
        if (mask && page < pages && !mask[page] && (page >= b.mask_acc.size() || !b.mask_acc[page]))
            continue; // the kernels left it as it was at the last copy-back
        if (g_profile)
            g_prof_rb_pages++;
        if (std::memcmp(now + pbeg, old + pbeg, pend - pbeg) == 0)
            continue;
        if (g_profile)
            g_prof_rb_changed_pages++;
        u32 off = pbeg;
        for (; off + 16 <= pend; off += 16) {
            const __m128i x = _mm_loadu_si128(reinterpret_cast<const __m128i*>(now + off));
            const __m128i y = _mm_loadu_si128(reinterpret_cast<const __m128i*>(old + off));
            const int eq = _mm_movemask_ps(_mm_castsi128_ps(_mm_cmpeq_epi32(x, y))); // bit i: word i unchanged
            if (eq == 0xF && run == ~0u)
                continue;
            for (int w = 0; w < 4; w++)
                word(off + 4u * w, (eq >> w) & 1);
        }
        for (; off + 4 <= pend; off += 4) {
            u32 p, q;
            std::memcpy(&p, now + off, 4);
            std::memcpy(&q, old + off, 4);
            word(off, p == q);
        }
        if (run != ~0u) // (runs end at page boundaries, as before)
            flush(off);
    }
    b.mask_acc.clear();
    b.apply_full = false;
    if (lo < hi) {
        textures_->Invalidate(b.addr + lo, hi - lo);
        for (auto& r : b.cpu_ranges) // our own writes dirtied the pages
            g.Mem().TrackWrites(r.cpu, r.size, 1), g_prof_track_bytes[1] += r.size;
    }
}

void D3D11Renderer::PollReadbacks(bool wait, const GpuBuf* until) {
    size_t need = wait ? readbacks_.size() : 0;
    if (until)
        for (size_t i = 0; i < readbacks_.size(); i++)
            if (readbacks_[i].b == until)
                need = std::max(need, i + 1);
    while (!readbacks_.empty()) {
        Readback& rb = readbacks_.front();
        // A newer copy of the same buffer supersedes this one (it contains every change made since).
        bool superseded = false;
        for (size_t i = 1; i < readbacks_.size() && !superseded; i++)
            superseded = readbacks_[i].b == rb.b;
        if (superseded) {
            // its changed pages still have to be compared when the newer copy is applied
            D3D11_MAPPED_SUBRESOURCE mm;
            if (!rb.mask || rb.b->apply_full)
                rb.b->apply_full = true;
            else if (SUCCEEDED(ctx_->Map(rb.mask.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mm))) {
                auto& acc = rb.b->mask_acc;
                if (acc.size() < rb.pages)
                    acc.resize(rb.pages, 0);
                const u32* f = static_cast<const u32*>(mm.pData);
                for (u32 p = 0; p < rb.pages; p++)
                    acc[p] |= f[p];
                ctx_->Unmap(rb.mask.Get(), 0);
            } else
                rb.b->apply_full = true;
        } else {
            // Not landed yet and nothing waits for it: ask the event (cheap) instead of trying to map (a driver call).
            if (!need && rb.done) {
                BOOL landed = FALSE;
                if (ctx_->GetData(rb.done.Get(), &landed, sizeof(landed), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_FALSE) {
                    if (g_profile)
                        g_prof_rb_not_ready++;
                    break;
                }
            }
            D3D11_MAPPED_SUBRESOURCE mp, mm;
            const HRESULT hr = ctx_->Map(rb.staging.Get(), 0, D3D11_MAP_READ, need ? 0 : D3D11_MAP_FLAG_DO_NOT_WAIT, &mp);
            if (hr == DXGI_ERROR_WAS_STILL_DRAWING)
                break;
            if (SUCCEEDED(hr)) {
                // (the flags were copied before the data: ready; without them every page is compared)
                const bool masked = rb.mask && SUCCEEDED(ctx_->Map(rb.mask.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mm));
                ApplyReadback(*rb.b, static_cast<const u8*>(mp.pData), rb.size, rb.serial,
                              masked ? static_cast<const u32*>(mm.pData) : nullptr, rb.pages);
                if (masked)
                    ctx_->Unmap(rb.mask.Get(), 0);
                ctx_->Unmap(rb.staging.Get(), 0);
            } else
                rb.b->apply_full = true; // (lost: prev already moved past it)
        }
        if (rb.mask)
            mask_pool_.push_back(std::move(rb.mask));
        if (rb.done)
            query_pool_.push_back(std::move(rb.done));
        rb.b->in_flight--;
        active_prune_ = true; // (finished buffers leave the active list at the next SyncGuest)
        staging_pool_.push_back(std::move(rb.staging));
        readbacks_.pop_front();
        if (need)
            need--;
    }
}

void D3D11Renderer::SyncGuest(u64 gpu_va, u64 size, int site) {
    if (!gpu_dirty_any_ && readbacks_.empty())
        return;
    sync_site_ = site;
    sync_va_ = gpu_va;
    // Only buffers with results in flight or not yet copied can need landing: a list of those, rebuilt when that
    // changes, instead of every resident buffer on each texture / constant / index lookup.
    if (active_stale_) { // a buffer was freed: list the active ones again
        active_.clear();
        for (auto& b : gbufs_) {
            b->listed = b->in_flight || b->gpu_dirty;
            if (b->listed)
                active_.push_back(b.get());
        }
        active_stale_ = false;
        active_prune_ = true; // (recomputes the bounds below)
    }
    if (active_prune_ && !in_sync_) { // copy-backs landed: drop the finished buffers, recompute the bounds
        size_t n = 0;
        active_lo_ = ~0ull, active_hi_ = 0;
        for (GpuBuf* b : active_) {
            if (b->in_flight || b->gpu_dirty) {
                active_[n++] = b;
                active_lo_ = std::min(active_lo_, b->addr);
                active_hi_ = std::max(active_hi_, b->addr + b->size);
            } else
                b->listed = false;
        }
        active_.resize(n);
        active_prune_ = false;
    }
    if (gpu_va >= active_hi_ || gpu_va + size <= active_lo_) { // (most lookups: nowhere near a compute buffer)
        sync_site_ = 0;
        return;
    }
    if (in_sync_) { // (re-entered: use a list of its own)
        const std::vector<GpuBuf*> list = active_;
        for (GpuBuf* b : list)
            if ((b->in_flight || b->gpu_dirty) && gpu_va < b->addr + b->size && b->addr < gpu_va + size)
                FlushGpuBuf(*b);
    } else {
        in_sync_ = true;
        for (size_t i = 0; i < active_.size(); i++) { // (FlushGpuBuf only marks the list stale, never rebuilds it)
            GpuBuf* b = active_[i];
            if ((b->in_flight || b->gpu_dirty) && gpu_va < b->addr + b->size && b->addr < gpu_va + size)
                FlushGpuBuf(*b);
        }
        in_sync_ = false;
    }
    sync_site_ = 0;
}

D3D11Renderer::GpuBuf* D3D11Renderer::ResidentBuffer(u64 addr, u64 size) {
    for (auto& bp : gbufs_) {
        GpuBuf& b = *bp;
        if (b.retired || !b.gpu_written || addr < b.addr || addr + size > b.addr + b.size)
            continue;
        if (ComputeBuffer(b.addr, b.size) != &b) // picks up CPU writes; never reallocates (size fits)
            return nullptr;
        return &b;
    }
    return nullptr;
}

bool D3D11Renderer::PrepareQuadIndirect(GpuBuf& args, u32 off) {
    if (!quad_args_cs_) {
        static const char* src =
            "ByteAddressBuffer a : register(t0); RWByteAddressBuffer o : register(u0); "
            "[numthreads(1,1,1)] void main() { uint4 v = a.Load4(0); o.Store4(0, uint4(v.x / 4 * 6, v.y, 0, v.z)); o.Store(16, v.w); }";
        ComPtr<ID3DBlob> code, err;
        if (FAILED(D3DCompile(src, std::strlen(src), nullptr, nullptr, nullptr, "main", "cs_5_0", 0, 0, &code, &err)) ||
            FAILED(dev_->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &quad_args_cs_)))
            return false;
        D3D11_BUFFER_DESC bd{32, D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS, 0,
                             D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS | D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS, 0};
        if (FAILED(dev_->CreateBuffer(&bd, nullptr, &quad_args_buf_)))
            return false;
        D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
        ud.Format = DXGI_FORMAT_R32_TYPELESS;
        ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        ud.Buffer = {0, 8, D3D11_BUFFER_UAV_FLAG_RAW};
        if (FAILED(dev_->CreateUnorderedAccessView(quad_args_buf_.Get(), &ud, &quad_args_uav_)))
            return false;
        constexpr u32 kQuads = 1u << 17;
        std::vector<u32> idx;
        idx.reserve(size_t(kQuads) * 6);
        for (u32 q = 0; q < kQuads; q++) {
            const u32 b = q * 4;
            idx.insert(idx.end(), {b, b + 1, b + 2, b, b + 2, b + 3});
        }
        D3D11_BUFFER_DESC id{u32(idx.size() * 4), D3D11_USAGE_IMMUTABLE, D3D11_BIND_INDEX_BUFFER, 0, 0, 0};
        D3D11_SUBRESOURCE_DATA sd{idx.data(), 0, 0};
        if (FAILED(dev_->CreateBuffer(&id, &sd, &quad_index_buf_)))
            return false;
    }
    auto& srv = args.srvs[u64(off) | (1ull << 63)]; // raw view of the 16 argument bytes
    if (!srv) {
        D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.Format = DXGI_FORMAT_R32_TYPELESS;
        sv.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
        sv.BufferEx = {off / 4, 4, D3D11_BUFFEREX_SRV_FLAG_RAW};
        if (FAILED(dev_->CreateShaderResourceView(args.buf.Get(), &sv, &srv)))
            return false;
    }
    ID3D11ShaderResourceView* s = srv.Get();
    ID3D11UnorderedAccessView* u = quad_args_uav_.Get();
    ctx_->CSSetShader(quad_args_cs_.Get(), nullptr, 0);
    ctx_->CSSetShaderResources(0, 1, &s);
    ctx_->CSSetUnorderedAccessViews(0, 1, &u, nullptr);
    ctx_->Dispatch(1, 1, 1);
    ID3D11ShaderResourceView* ns = nullptr;
    ID3D11UnorderedAccessView* nu = nullptr;
    ctx_->CSSetShaderResources(0, 1, &ns);
    ctx_->CSSetUnorderedAccessViews(0, 1, &nu, nullptr);
    return true;
}

ID3D11ShaderResourceView* D3D11Renderer::ComputeBufferSrv(u64 gpu_va, u64 size, DXGI_FORMAT fmt, u32 bpb) {
    if (!bpb || !size || fmt == DXGI_FORMAT_UNKNOWN)
        return nullptr;
    for (auto& bp : gbufs_) {
        GpuBuf& b = *bp;
        if (b.retired || !b.gpu_written || gpu_va < b.addr || gpu_va + size > b.addr + b.size)
            continue;
        const u64 off = gpu_va - b.addr;
        if (off % bpb)
            return nullptr;
        if (ComputeBuffer(b.addr, b.size) != &b) // picks up CPU writes; never reallocates (size fits)
            return nullptr;
        const u64 key = off | (size << 27) | (u64(fmt) << 56);
        auto& srv = b.srvs[key];
        if (!srv) {
            D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
            sv.Format = fmt;
            sv.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
            sv.Buffer.FirstElement = u32(off / bpb);
            sv.Buffer.NumElements = u32(size / bpb);
            if (FAILED(dev_->CreateShaderResourceView(b.buf.Get(), &sv, &srv)))
                return nullptr;
        }
        return srv.Get();
    }
    return nullptr;
}

bool D3D11Renderer::RefreshGeometry(GeomBuf& b, D3D11_BIND_FLAG bind, bool& fill) {
    auto& g = G();
    b.last_use = frame_counter_;
    if (b.streaming)
        return false;
    if (!b.buf) { // only cache data drawn again in a later frame (not one-off ring-buffer contents)
        if (b.first_seen == ~0ull || b.first_seen == frame_counter_) {
            b.first_seen = frame_counter_;
            return false;
        }
    }
    SyncGuest(b.addr, b.size, 7);
    fill = !b.buf;
    std::vector<std::pair<u32, u32>> runs;
    if (!fill) {
        CollectCpuWrites(b.cpu_ranges, runs, 2);
        if (runs.empty()) {
            b.dirty_streak = 0;
            return true;
        }
        if (++b.dirty_streak >= 8) { // rewritten every frame: stop tracking it
            b.streaming = true;
            b.buf.Reset();
            return false;
        }
        if (bind == D3D11_BIND_INDEX_BUFFER)
            fill = true; // small, and the vertex range has to be recomputed anyway
    } else {
        D3D11_BUFFER_DESC bd{b.size, D3D11_USAGE_DEFAULT, UINT(bind), 0, 0, 0};
        if (FAILED(dev_->CreateBuffer(&bd, nullptr, &b.buf)))
            return false;
        b.cpu_ranges.clear();
        for (u64 off = 0; off < b.size;) {
            u64 run = 0;
            const u64 cpu = g.As().TranslateRun(b.addr + off, run);
            if (!cpu || !run)
                break;
            const u64 n = std::min<u64>(run, b.size - off);
            b.cpu_ranges.push_back({u32(off), cpu, n});
            off += n;
        }
    }
    if (!fill) {
        static thread_local std::vector<u8> tmp;
        for (auto [off, n] : runs) {
            n = std::min(n, b.size - off);
            tmp.resize(n);
            g.ReadGpu(b.addr + off, tmp.data(), n);
            const D3D11_BOX box{off, 0, 0, off + n, 1, 1};
            ctx_->UpdateSubresource(b.buf.Get(), 0, &box, tmp.data(), n, n);
        }
    }
    for (auto& r : b.cpu_ranges)
        g.Mem().TrackWrites(r.cpu, r.size, 2), g_prof_track_bytes[2] += r.size;
    return true;
}

D3D11Renderer::GeomBuf* D3D11Renderer::CachedIndices(u64 addr, u32 count, u32 index_size, bool strip) {
    if (!count || count > (16u << 20))
        return nullptr;
    const u64 key = addr ^ (u64(count) << 34) ^ (u64(index_size) << 61) ^ (strip ? 1ull << 63 : 0);
    GeomBuf& b = index_cache_[key];
    const u32 size = u32(common::AlignUp(count * std::max<u32>(index_size, 2), 4));
    if (b.addr != addr || b.size != size || b.index_size != index_size) { // new, or a key collision
        b = {};
        b.addr = addr;
        b.size = size;
        b.index_size = index_size;
    }
    bool fill = false;
    if (!RefreshGeometry(b, D3D11_BIND_INDEX_BUFFER, fill))
        return nullptr;
    if (fill) {
        std::vector<u8> raw(size_t(count) * index_size);
        G().ReadGpu(addr, raw.data(), raw.size());
        std::vector<u8> data(b.size, 0);
        u32 lo = ~0u, hi = 0;
        const u32 restart = index_size == 4 ? ~0u : 0xFFFF;
        for (u32 i = 0; i < count; i++) {
            const u32 v = index_size == 1 ? raw[i] : index_size == 2 ? common::Read<u16>(&raw[i * 2]) : common::Read<u32>(&raw[i * 4]);
            if (index_size == 1)
                { const u16 v16 = u16(v); std::memcpy(&data[i * 2], &v16, 2); }
            if (strip && v == restart && index_size != 1)
                continue; // D3D cuts strips at all-ones indices, like Maxwell's primitive restart
            lo = std::min(lo, v);
            hi = std::max(hi, v);
        }
        if (index_size != 1)
            std::memcpy(data.data(), raw.data(), raw.size());
        b.index_format = index_size == 4 ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT;
        b.min_v = lo == ~0u ? 0 : lo;
        b.max_v = lo == ~0u ? 0 : hi;
        const D3D11_BOX box{0, 0, 0, b.size, 1, 1};
        ctx_->UpdateSubresource(b.buf.Get(), 0, &box, data.data(), b.size, b.size);
    }
    return &b;
}

D3D11Renderer::GeomBuf* D3D11Renderer::CachedVertices(u64 addr, u32 size) {
    GeomBuf& b = vertex_cache_[addr];
    if (b.size < size) { // new or grew: start over
        const u64 seen = b.first_seen;
        b = {};
        b.first_seen = seen;
        b.addr = addr;
        b.size = u32(common::AlignUp(size, 16));
    }
    bool fill = false;
    if (!RefreshGeometry(b, D3D11_BIND_VERTEX_BUFFER, fill))
        return nullptr;
    if (fill) {
        std::vector<u8> data(b.size, 0);
        G().ReadGpuPartial(addr, data.data(), std::min(size, b.size));
        const D3D11_BOX box{0, 0, 0, b.size, 1, 1};
        ctx_->UpdateSubresource(b.buf.Get(), 0, &box, data.data(), b.size, b.size);
    }
    return &b;
}

// Synchronous copy of a GPU buffer's first `size` bytes (debug paths only).
static std::vector<u8> ReadBufferNow(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11Buffer* buf, u32 size) {
    std::vector<u8> out;
    D3D11_BUFFER_DESC sd{size, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0, 0};
    ComPtr<ID3D11Buffer> st;
    if (FAILED(dev->CreateBuffer(&sd, nullptr, &st)))
        return out;
    const D3D11_BOX box{0, 0, 0, size, 1, 1};
    ctx->CopySubresourceRegion(st.Get(), 0, 0, 0, 0, buf, 0, &box);
    D3D11_MAPPED_SUBRESOURCE mp;
    if (SUCCEEDED(ctx->Map(st.Get(), 0, D3D11_MAP_READ, 0, &mp))) {
        out.assign(static_cast<const u8*>(mp.pData), static_cast<const u8*>(mp.pData) + size);
        ctx->Unmap(st.Get(), 0);
    }
    return out;
}

// MUA3's culling geometry program: for each visible triangle it atomically reserves three slots in an index list
// (counter address at GS c0[0x410], list base at c0[0x420]) and stores the vertex ids as v1, v0, v2. Later draws
// index that list. We keep every triangle (culling is only an optimisation).

void D3D11Renderer::Dispatch(const ComputeLaunch& l) {
    std::lock_guard ctx_lock(ContextMutex());
    auto& g = G();
    auto& dc = *g_dc;
    // ---- program and launch shape
    const u64 prog = l.program;
    shader::TranslateOptions opt;
    opt.block[0] = l.block[0];
    opt.block[1] = l.block[1];
    opt.block[2] = l.block[2];
    opt.shared_bytes = l.shared_bytes;
    const u32* const grid = l.grid;
    if (!grid[0] || !grid[1] || !grid[2])
        return;
    // Debug: MUA3_SKIP_CS=<offset hex>,... drops those compute programs.
    static const std::string skip_cs = [] {
        const char* e = std::getenv("MUA3_SKIP_CS");
        return std::string(e ? e : "");
    }();
    if (!skip_cs.empty() && skip_cs.find(fmt::format("{:x}", l.program_offset)) != std::string::npos)
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
        if (l.cb_size[1]) { // jump tables for BRX
            cb1.assign(l.cb_size[1], 0);
            g.ReadGpuPartial(l.cb_addr[1], cb1.data(), cb1.size());
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
    const u64* const cb_addr = l.cb_addr;
    const u32* const cb_size = l.cb_size;
    // Integer-typed textures need a shader variant, as for draws.
    const u64 tic_base = l.tic_base, tsc_base = l.tsc_base;
    const u32 tex_cb = kTextureHandleCb;
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
    static common::U64Map<int> logged;
    const int nth = logged[key]++;
    const bool log = nth < 2; // (every 600th as well wrote ~10 lines a second from this thread)
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
        drv.swizzle[t.slot][1] = TextureScale(tic.Address(), t.dim == shader::TexDim::Tex2D && !t.array);
        if (t.sampler < 16)
            smps[t.sampler] = textures_->GetSampler(tsc, t.shadow);
    }
    // ---- global memory windows: NVN's storage buffer table in c0 at 0x310 ({address lo, hi, size, -} each)
    PollReadbacks(false);
    ID3D11UnorderedAccessView* uavs[shader::kGmemWindows]{};
    GpuBuf* win[shader::kGmemWindows]{};
    u32 win_size[shader::kGmemWindows]{};
    if (cs->info.uses_gmem && cb_size[0] >= 0x310 + 16 * shader::kGmemWindows) {
        u32 table[shader::kGmemWindows][4];
        g.ReadGpu(cb_addr[0] + 0x310, table, sizeof(table));
        for (u32 i = 0; i < shader::kGmemWindows; i++) {
            const u64 a = (u64(table[i][1]) << 32) | table[i][0];
            const u32 size = u32(common::AlignUp(table[i][2], 4));
            if (!a || !size || size > (64u << 20))
                continue;
            GpuBuf* w = ComputeBuffer(a, size);
            if (!w)
                continue;
            uavs[i] = w->uav.Get();
            win[i] = w;
            win_size[i] = w->size; // (the buffer may be a larger one containing this window: its own range)
            drv.gmem[i][0] = u32(w->addr);
            drv.gmem[i][1] = u32(w->addr >> 32);
            drv.gmem[i][2] = w->size;
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
                       : fmt == 0x0C && rt == 7 ? 5u : fmt == 0x04 ? 6u : fmt == 0x21 ? 7u : 0u;
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
        GpuBuf* gw = ComputeBuffer(a, u32(size));
        if (!gw)
            continue;
        const u32 i = next_window++;
        uavs[i] = gw->uav.Get();
        win[i] = gw;
        win_size[i] = gw->size;
        drv.gmem[i][0] = u32(gw->addr);
        drv.gmem[i][1] = u32(gw->addr >> 32);
        drv.gmem[i][2] = gw->size;
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
    if (!captured && l.program_offset == capture_off) {
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
                const auto d = ReadBufferNow(dev_, ctx_, win[i]->buf.Get(), win_size[i]);
                put_bytes(d.data(), u32(d.size()));
            }
        put_bytes(&drv, sizeof(drv));
        const fs::path out = fmt::format("shader_dump/capture_{:x}.bin", l.program_offset);
        common::WriteFile(out, std::span(reinterpret_cast<const u8*>(f.data()), f.size() * 4));
        LOG_INFO("Renderer", "captured dispatch {:#x} to {}", prog, out.string());
    }
    ID3D11Buffer* drv_cb = dc.Constant(1001, &drv, sizeof(drv));
    // Render targets may be sampled by the kernel: unbind them from the output stage.
    ctx_->OMSetRenderTargets(0, nullptr, nullptr);
    std::memset(last_rtvs_, 0xFF, sizeof(last_rtvs_)); // the next draw binds its targets again
    last_dsv_ = reinterpret_cast<ID3D11DepthStencilView*>(~uintptr_t(0));
    InvalidateBinds(); // UAVs bound below unbind VS/PS views of the same resources
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
        std::vector<std::vector<u8>> before(shader::kGmemWindows);
        for (u32 i = 0; i < shader::kGmemWindows; i++)
            if (win[i])
                before[i] = ReadBufferNow(dev_, ctx_, win[i]->buf.Get(), win_size[i]);
        ctx_->CSSetShader(cs->cs_ref.Get(), nullptr, 0);
        ctx_->Dispatch(grid[0], grid[1], grid[2]);
        for (u32 i = 0; i < shader::kGmemWindows; i++) {
            if (!win[i] || before[i].size() != win_size[i])
                continue;
            reference[i] = ReadBufferNow(dev_, ctx_, win[i]->buf.Get(), win_size[i]);
            const D3D11_BOX box{0, 0, 0, win_size[i], 1, 1};
            ctx_->UpdateSubresource(win[i]->buf.Get(), 0, &box, before[i].data(), win_size[i], win_size[i]);
        }
        ctx_->CSSetShader(cs->cs.Get(), nullptr, 0);
    }
    ctx_->Dispatch(grid[0], grid[1], grid[2]);
    ID3D11UnorderedAccessView* no_uav[shader::kGmemWindows]{};
    ctx_->CSSetUnorderedAccessViews(0, shader::kGmemWindows, no_uav, nullptr);
    ID3D11ShaderResourceView* no_srv[48]{};
    ctx_->CSSetShaderResources(0, 48, no_srv);
    dispatches_++;
    // ---- results stay on the GPU (the CPU and host-side reads see them after the copy-back, see Present/SyncGuest)
    for (u32 i = 0; i < shader::kGmemWindows && cs->info.writes_gmem; i++) {
        GpuBuf* w = win[i];
        if (!w || std::find(win, win + i, w) != win + i)
            continue;
        if (verify && reference[i].size() == win_size[i]) {
            const auto now = ReadBufferNow(dev_, ctx_, w->buf.Get(), win_size[i]);
            u32 diff = 0, first = ~0u;
            for (u32 off = 0; off + 4 <= now.size(); off += 4)
                if (std::memcmp(&now[off], &reference[i][off], 4) != 0) {
                    diff++;
                    if (first == ~0u)
                        first = off;
                }
            static std::map<u64, u32> reported;
            if (diff && reported[key ^ i]++ < 3) {
                u32 a4 = 0, b4 = 0;
                std::memcpy(&a4, &now[first], 4);
                std::memcpy(&b4, &reference[i][first], 4);
                LOG_WARN("Renderer", "DXBC verify {:#x} window {}: {} words differ (first +{:#x}: dxbc {:08x} fxc {:08x})", prog, i,
                         diff, first, a4, b4);
            } else if (!diff && reported[key ^ i] == 0) {
                reported[key ^ i] = 100;
                LOG_INFO("Renderer", "DXBC verify {:#x} window {}: identical", prog, i);
            }
        }
        // Results stay on the GPU; Present copies dirty buffers back to guest memory once per frame.
        w->gpu_written = true;
        w->gpu_dirty = true;
        gpu_dirty_any_ = true;
        MarkActive(*w);
    }
}

void D3D11Renderer::EmulateCullingGs() {
    auto& g = G();
    const auto& m = g.State();
    const auto& dr = m.draw;
    const auto& cb = m.cb[kCbGeometry][0];
    if (!cb.enabled || cb.size < 0x428 || dr.topology != 4)
        return;
    u32 c0[2], base[2];
    g.ReadGpu(cb.address + 0x410, c0, 8);
    g.ReadGpu(cb.address + 0x420, base, 8);
    const u64 counter_va = u64(c0[0]) | (u64(c0[1]) << 32), list_va = u64(base[0]) | (u64(base[1]) << 32);
    if (!counter_va || !list_va)
        return;
    const u32 tris = dr.count / 3;
    if (!tris)
        return;
    const size_t bytes = size_t(tris) * 12;
    // The emulated program keeps every triangle, so the list is the draw's own indices (as u32). Indexed draws only
    // remember where those indices are: the draw that consumes the list reads them from there (copying and comparing
    // every list cost the GPU thread ~7% in fights). MUA3_CULL_COPY=1 writes the lists as before.
    static const bool copy_lists = std::getenv("MUA3_CULL_COPY") != nullptr;
    if (dr.indexed && !copy_lists && trace_draws_ <= 0) {
        const u32 fmt = m.index_format;
        const u32 isz = fmt == 0 ? 1 : fmt == 1 ? 2 : 4;
        u32 counter = 0;
        SyncGuest(counter_va, 4, 3);
        g.ReadGpu(counter_va, &counter, 4);
        const u64 list = list_va + u64(counter) * 4;
        if (cull_lists_.size() > 65536)
            cull_lists_.clear(), cull_src_.clear();
        cull_lists_[list] = tris * 3;
        cull_src_[list] = {m.index_addr + u64(dr.first) * isz, isz, tris * 3};
        counter += tris * 3;
        g.WriteGpu(counter_va, &counter, 4);
        return;
    }
    static thread_local std::vector<u32> out;
    const u8* src = nullptr;
    if (dr.indexed) {
        const u32 fmt = m.index_format;
        const u32 isz = fmt == 0 ? 1 : fmt == 1 ? 2 : 4;
        const u64 ia = m.index_addr + u64(dr.first) * isz;
        SyncGuest(ia, size_t(tris) * 3 * isz, 2);
        if (isz == 4)
            src = g.GpuPtr(ia, bytes);
        if (!src) {
            static thread_local std::vector<u8> raw;
            raw.resize(size_t(tris) * 3 * isz);
            g.ReadGpu(ia, raw.data(), raw.size());
            out.resize(size_t(tris) * 3);
            for (u32 i = 0; i < tris * 3; i++)
                out[i] = isz == 1 ? raw[i] : isz == 2 ? common::Read<u16>(&raw[i * 2]) : common::Read<u32>(&raw[i * 4]);
            src = reinterpret_cast<const u8*>(out.data());
        }
    } else {
        out.resize(size_t(tris) * 3);
        for (u32 i = 0; i < tris * 3; i++)
            out[i] = dr.first + i;
        src = reinterpret_cast<const u8*>(out.data());
    }
    u32 counter = 0;
    SyncGuest(counter_va, 4, 3);
    g.ReadGpu(counter_va, &counter, 4);
    if (trace_draws_ > 0)
        LOG_INFO("Renderer", "TRACE culling-gs: counter {:#x} = {} list {:#x} +{} tris", counter_va, counter, list_va, tris);
    // Usually the same list as last frame: leave identical data alone so cached index buffers over it stay valid.
    const u8* cur = g.GpuPtr(list_va + u64(counter) * 4, bytes);
    if (!cur || std::memcmp(cur, src, bytes) != 0)
        g.WriteGpu(list_va + u64(counter) * 4, src, bytes);
    if (cull_lists_.size() > 65536)
        cull_lists_.clear(), cull_src_.clear();
    cull_lists_[list_va + u64(counter) * 4] = tris * 3;
    cull_src_.erase(list_va + u64(counter) * 4);
    counter += tris * 3;
    g.WriteGpu(counter_va, &counter, 4);
}

void D3D11Renderer::Draw() {
    std::lock_guard ctx_lock(ContextMutex());
    auto& g = G();
    const auto& m = g.State();
    auto& dc = *g_dc;
    // Someone else used the context (UI) or a dispatch rebound resources: forget what is bound.
    if (const u64 epoch = ContextEpoch().load(); epoch != binds_epoch_) {
        binds_epoch_ = epoch;
        InvalidatePipe();
        for (auto& b : binds_)
            std::memset(&b, 0xFF, sizeof(b));
        std::fill(std::begin(last_rtvs_), std::end(last_rtvs_), reinterpret_cast<ID3D11RenderTargetView*>(~uintptr_t(0)));
        last_dsv_ = reinterpret_cast<ID3D11DepthStencilView*>(~uintptr_t(0));
    }
    if (!readbacks_.empty() && ++draws_since_poll_ >= 32) {
        draws_since_poll_ = 0;
        PollReadbacks(false);
    }
    struct ProfEnd {
        ~ProfEnd() { Prof(-1); }
    } prof_end;
    Prof(kProfShaders);
    if (g_profile)
        g_prof_draws++;

    // ---- shaders
    auto load_shader = [&](u64 addr, shader::Stage stage) -> HostShader* {
        if (!addr)
            return nullptr;
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
    HostShader* vs = load_shader(m.program[kStageVertex], shader::Stage::Vertex);
    HostShader* ps = load_shader(m.program[kStageFragment], shader::Stage::Fragment);
    // A geometry program without any OUT (emit) instruction rasterizes nothing: MUA3 uses one to cull triangles
    // into a buffer ahead of its real draws. Drawing such a pass would leave depth the game never wrote.
    if (const u64 addr = m.program[kStageGeometry]) {
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
    for (u32 prog : {u32(kStageTessControl), u32(kStageTessEval), u32(kStageGeometry)}) {
        const u64 addr = m.program[prog];
        if (!addr)
            continue;
        static std::set<u64> dumped;
        if (!dumped.insert(addr).second)
            continue;
        std::vector<u8> buf(0x10000, 0);
        const size_t got = g.ReadGpuPartial(addr, buf.data(), buf.size());
        buf.resize(shader::ProgramSize(buf.data(), got));
        common::WriteFile(fmt::format("shader_dump/{:016x}.stage{}.bin", Hash64(buf.data(), buf.size()), prog), buf);
        LOG_INFO("Renderer", "stage {} program enabled (not run), dumped {} bytes", prog, buf.size());
    }
    // Integer vertex attributes and integer textures need integer-typed shader declarations (a float4
    // declaration would receive converted values): pick the matching shader variant.
    // This draw's texture handles and descriptors per stage (0 vertex, 1 fragment), read once. The game rebuilds its
    // handles every frame, so they can't identify a texture from one draw or frame to the next.
    struct StageTexture {
        u32 handle;
        TicEntry tic;
    };
    static thread_local StageTexture stage_textures[2][32];
    bool stage_read[2] = {false, false};
    auto read_stage_textures = [&](HostShader* sh, u32 stage_index) {
        const u32 st = stage_index == kCbVertex ? 0 : 1;
        if (stage_read[st])
            return;
        stage_read[st] = true;
        const auto& tcb = m.cb[stage_index][kTextureHandleCb];
        for (auto& t : sh->info.textures) {
            if (t.slot >= 32)
                continue;
            StageTexture& e = stage_textures[st][t.slot];
            e.handle = 0;
            g.ReadGpu(tcb.address + t.cbuf_offset, &e.handle, 4);
            g.ReadGpu(m.tic_base + u64(e.handle & 0xFFFFF) * 32, e.tic.w.data(), 32);
        }
    };
    auto int_textures = [&](HostShader* sh, u32 stage_index, ShaderCache::VariantKey& key) {
        read_stage_textures(sh, stage_index);
        const u32 st = stage_index == kCbVertex ? 0 : 1;
        for (auto& t : sh->info.textures) {
            if (t.slot >= 32)
                continue;
            const TicEntry& tic = stage_textures[st][t.slot].tic;
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
        if (g_seen.attr_vs != vs->hash || g_seen.attr_serial != m.vertex_format_serial) {
            ShaderCache::VariantKey key;
            for (u32 i = 0; i < 32; i++) {
                if (!(vs->info.attrib_in_mask & (1u << i)))
                    continue;
                const u32 f = m.attrib[i];
                const u32 b = f & 0x1F;
                if ((f & 0x40) || b >= 16 || !m.streams[b].enabled || AttribFormat(f) == DXGI_FORMAT_UNKNOWN)
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
            g_seen.attr_vs = vs->hash;
            g_seen.attr_serial = m.vertex_format_serial;
            g_seen.attr_key = key;
        }
        ShaderCache::VariantKey key = g_seen.attr_key;
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
    // A shader the game just asked for is at the front of the compile queue: wait a moment for it (from the
    // bytecode cache that is milliseconds) rather than leave the draw out of the frame.
    for (HostShader* sh : {vs, ps})
        if (sh && sh->state == HostShader::Pending && shader_wait_ns_ > 0) {
            const auto t0 = std::chrono::steady_clock::now();
            shaders_->WaitReady(sh, std::chrono::nanoseconds(shader_wait_ns_));
            shader_wait_ns_ -= s64((std::chrono::steady_clock::now() - t0).count());
        }
    for (HostShader* sh : {vs, ps}) // the driver object is made at first use and dropped again when unused
        if (sh) {
            sh->last_used = shaders_->frame;
            if (sh->state == HostShader::Ready && !sh->created.load(std::memory_order_acquire))
                shaders_->Realize(sh);
        }
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
            for (u32 i = 0; i < kNumGraphicsStages; i++)
                LOG_WARN("Renderer", "  stage {}: {:#x}", i, m.program[i]);
        }
        return;
    }

    // ---- render targets
    Prof(kProfState);
    Surface* colors[8]{};
    const u32 count = m.rt_count;
    for (u32 i = 0; i < count; i++)
        colors[i] = GetColorSurface(i);
    Surface* depth = GetDepthSurface();
    ID3D11RenderTargetView* rtvs[8]{};
    for (u32 i = 0; i < count; i++)
        rtvs[i] = colors[i] ? colors[i]->rtv.Get() : nullptr;
    ID3D11DepthStencilView* dsv = depth ? depth->dsv.Get() : nullptr;
    // Binding outputs makes D3D silently unbind those resources' SRVs: start the SRV cache over then.
    if (std::memcmp(last_rtvs_, rtvs, sizeof(rtvs)) != 0 || last_dsv_ != dsv) {
        std::memcpy(last_rtvs_, rtvs, sizeof(rtvs));
        last_dsv_ = dsv;
        for (auto& b : binds_)
            std::fill(std::begin(b.srv), std::end(b.srv), reinterpret_cast<ID3D11ShaderResourceView*>(~uintptr_t(0)));
        ctx_->OMSetRenderTargets(8, rtvs, dsv);
    }
    if (!pipe_.valid || debug_mode_ >= 1)
        InvalidatePipe();
    draw_serial_++;
    for (u32 i = 0; i < 8; i++)
        bound_[i] = i < count ? colors[i] : nullptr;
    bound_[8] = depth;

    // ---- viewport
    const float sx = m.vp_scale[0], sy = m.vp_scale[1], sz = m.vp_scale[2];
    const float tx = m.vp_translate[0], ty = m.vp_translate[1], tz = m.vp_translate[2];
    // Targets stored at a higher resolution scale the viewport and scissor with them.
    u32 rscale = 1;
    for (Surface* b : bound_)
        if (b) {
            rscale = b->scale;
            break;
        }
    const float fs = float(rscale);
    D3D11_VIEWPORT vp{(tx - std::abs(sx)) * fs, (ty - std::abs(sy)) * fs, 2 * std::abs(sx) * fs, 2 * std::abs(sy) * fs,
                      0.0f, 1.0f};
    if (std::memcmp(&pipe_.vp, &vp, sizeof(vp)) != 0) {
        pipe_.vp = vp;
        ctx_->RSSetViewports(1, &vp);
    }
    const float flip = sy > 0 ? -1.0f : 1.0f;

    // ---- scissor
    D3D11_RECT sr{0, 0, 16384, 16384};
    if (m.scissor_enable)
        sr = {LONG(m.scissor[0]) * LONG(rscale), LONG(m.scissor[1]) * LONG(rscale), LONG(m.scissor[2]) * LONG(rscale), LONG(m.scissor[3]) * LONG(rscale)};
    // Pixel history (debug "pick x y"): restrict every draw of one frame to that pixel and count samples.
    const bool counting = occl_frame_ == frame_counter_; // debug "occl": samples written by every draw
    const bool picking = (pick_frame_ == frame_counter_ && colors[0] && colors[0]->width >= 1280 / 2) || counting;
    if (picking && !counting) {
        const u32 px = u32(pick_x_ * colors[0]->width * rscale), py = u32(pick_y_ * colors[0]->height * rscale);
        sr = {std::max<LONG>(sr.left, LONG(px)), std::max<LONG>(sr.top, LONG(py)), std::min<LONG>(sr.right, LONG(px + 1)),
              std::min<LONG>(sr.bottom, LONG(py + 1))};
    }
    if (std::memcmp(&pipe_.scissor, &sr, sizeof(sr)) != 0) {
        pipe_.scissor = sr;
        ctx_->RSSetScissorRects(1, &sr);
    }

    // ---- blend
    if (g_seen.blend != m.blend_serial) {
        g_seen.blend = m.blend_serial;
        D3D11_BLEND_DESC bd{};
        bd.IndependentBlendEnable = TRUE;
        for (u32 i = 0; i < 8; i++) {
            auto& t = bd.RenderTarget[i];
            const auto& b = m.blend[i];
            t.BlendEnable = m.blend_enable[i];
            t.BlendOp = BlendOp(b.eq_rgb);
            t.SrcBlend = BlendFactor(b.src_rgb);
            t.DestBlend = BlendFactor(b.dst_rgb);
            t.BlendOpAlpha = BlendOp(b.eq_a);
            t.SrcBlendAlpha = AlphaFactor(BlendFactor(b.src_a));
            t.DestBlendAlpha = AlphaFactor(BlendFactor(b.dst_a));
            t.RenderTargetWriteMask = u8(m.color_mask[i]);
        }
        g_seen.blend_obj = dc.Cached(dc.blend, bd, [&](const D3D11_BLEND_DESC* d, ID3D11BlendState** o) {
            dev_->CreateBlendState(d, o);
        });
    }
    ID3D11BlendState* const blend = g_seen.blend_obj;
    const float* const bc = m.blend_color;
    if (pipe_.blend != blend || std::memcmp(pipe_.blend_color, bc, sizeof(pipe_.blend_color)) != 0) {
        pipe_.blend = blend;
        std::memcpy(pipe_.blend_color, bc, sizeof(pipe_.blend_color));
        ctx_->OMSetBlendState(blend, bc, 0xFFFFFFFF);
    }

    // ---- depth / stencil
    if (g_debug_ps_state || g_seen.depth != m.depth_serial || g_seen.depth_on != (depth != nullptr)) {
        g_seen.depth = m.depth_serial;
        g_seen.depth_on = depth != nullptr;
        D3D11_DEPTH_STENCIL_DESC dd{};
        dd.DepthEnable = depth && m.depth_test;
        dd.DepthWriteMask = m.depth_write ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
        dd.DepthFunc = CompareFunc(m.depth_func);
        dd.StencilEnable = depth && m.stencil_test;
        dd.StencilReadMask = u8(m.stencil_read_mask);
        dd.StencilWriteMask = u8(m.stencil_write_mask);
        dd.FrontFace = {StencilOp(m.stencil_front.op_fail), StencilOp(m.stencil_front.op_zfail),
                        StencilOp(m.stencil_front.op_zpass), CompareFunc(m.stencil_front.func)};
        dd.BackFace = {StencilOp(m.stencil_back.op_fail), StencilOp(m.stencil_back.op_zfail),
                       StencilOp(m.stencil_back.op_zpass), CompareFunc(m.stencil_back.func)};
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
        g_seen.depth_obj = dc.Cached(dc.depth, dd, [&](const D3D11_DEPTH_STENCIL_DESC* d, ID3D11DepthStencilState** o) {
            dev_->CreateDepthStencilState(d, o);
        });
    }
    ID3D11DepthStencilState* const dss = g_seen.depth_obj;
    if (const u32 sref = m.stencil_ref & 0xFF; pipe_.dss != dss || pipe_.stencil_ref != sref) {
        pipe_.dss = dss;
        pipe_.stencil_ref = sref;
        ctx_->OMSetDepthStencilState(dss, sref);
    }

    // ---- rasterizer
    if (g_debug_ps_state || g_seen.raster != m.raster_serial || g_seen.flip != flip) {
        g_seen.raster = m.raster_serial;
        g_seen.flip = flip;
        D3D11_RASTERIZER_DESC rd{};
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = D3D11_CULL_NONE;
        if (m.cull_enable) {
            const u32 cf = m.cull_face;
            rd.CullMode = cf == 0x404 ? D3D11_CULL_FRONT : cf == 0x405 ? D3D11_CULL_BACK : D3D11_CULL_NONE;
        }
        bool ccw = m.front_ccw;
        if (flip < 0)
            ccw = !ccw;
        rd.FrontCounterClockwise = ccw;
        rd.DepthClipEnable = TRUE;
        // Polygon offset (shadow casters).
        if (m.polygon_offset) {
            rd.DepthBias = INT(std::lround(m.depth_bias));
            rd.SlopeScaledDepthBias = m.slope_scale_depth_bias;
            rd.DepthBiasClamp = m.depth_bias_clamp;
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
        g_seen.raster_obj = dc.Cached(dc.raster, rd, [&](const D3D11_RASTERIZER_DESC* d, ID3D11RasterizerState** o) {
            dev_->CreateRasterizerState(d, o);
        });
    }
    ID3D11RasterizerState* const rs = g_seen.raster_obj;
    if (pipe_.rs != rs) {
        pipe_.rs = rs;
        ctx_->RSSetState(rs);
    }

    // ---- topology / indices
    Prof(kProfIndex);
    auto dr = m.draw;
    std::vector<u32> converted;
    u32 index_size = 0;
    u32 min_v = 0, max_v = 0;
    D3D11_PRIMITIVE_TOPOLOGY topo = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    std::vector<u32> indices;
    u32 index_count = 0;
    bool direct = false; // non-indexed list/strip: Draw() straight from the vertex buffers
    bool whole_vb = false; // the vertex range is unknown (indices stay on the GPU): bind whole vertex buffers
    const bool native_topology = dr.topology == 0 || dr.topology == 1 || dr.topology == 3 || dr.topology == 4 || dr.topology == 5;
    // Indirect arguments: drawn from the resident compute copy when they are one (the vertex range is then
    // unknown, so whole vertex buffers are bound); otherwise read, waiting for the GPU to produce them.
    GpuBuf* indirect = nullptr;
    bool indirect_quads = false;
    if (dr.indirect_args) {
        if ((native_topology || dr.topology == 7) && !dr.indexed && trace_draws_ <= 0 && !trace_draw_)
            indirect = ResidentBuffer(dr.indirect_args, 16);
        if (indirect && dr.topology == 7 && !PrepareQuadIndirect(*indirect, u32(dr.indirect_args - indirect->addr)))
            indirect = nullptr;
        if (indirect) {
            dr.first = 0;
            dr.count = 1;
            whole_vb = true;
            indirect_quads = dr.topology == 7;
            g_prof_resident++;
        } else {
            u32 a[4]{}; // count, instances, first, base instance
            SyncGuest(dr.indirect_args, sizeof(a), 11);
            g.ReadGpu(dr.indirect_args, a, sizeof(a));
            if (!a[0] || !a[1])
                return;
            dr.first = a[2];
            dr.count = a[0];
        }
    }
    if (indirect_quads) { // the fixed quad index list; the GPU converted the arguments
        topo = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        ctx_->IASetPrimitiveTopology(topo);
        ctx_->IASetIndexBuffer(quad_index_buf_.Get(), DXGI_FORMAT_R32_UINT, 0);
        index_count = 1;
    } else if (native_topology && dr.count && trace_draws_ <= 0 && !trace_draw_) {
        static const D3D11_PRIMITIVE_TOPOLOGY kTopology[6] = {
            D3D11_PRIMITIVE_TOPOLOGY_POINTLIST, D3D11_PRIMITIVE_TOPOLOGY_LINELIST, D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED,
            D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP};
        topo = kTopology[dr.topology];
        if (!dr.indexed) {
            direct = true;
            index_count = dr.count;
            min_v = dr.first;
            max_v = dr.first + dr.count - 1;
        } else {
            const u32 fmt = m.index_format;
            index_size = fmt == 0 ? 1 : fmt == 1 ? 2 : 4;
            const bool strip = dr.topology == 3 || dr.topology == 5;
            u64 ia = m.index_addr + u64(dr.first) * index_size;
            if (index_size == 4 && !cull_src_.empty()) // a culling list (EmulateCullingGs): its source indices
                if (auto cs = cull_src_.find(ia); cs != cull_src_.end() && cs->second.count == dr.count) {
                    ia = cs->second.addr;
                    index_size = cs->second.index_size;
                }
            GpuBuf* gb = index_size > 1 && !strip ? ResidentBuffer(ia, u64(dr.count) * index_size) : nullptr;
            if (gb && (ia - gb->addr) % index_size == 0) {
                // compute-written indices (the culling list): drawn from the resident copy; the vertex range is
                // unknown on the host, so per-draw vertex uploads take whole buffers
                ctx_->IASetIndexBuffer(gb->buf.Get(), index_size == 2 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT, u32(ia - gb->addr));
                index_count = dr.count;
                whole_vb = true;
                g_prof_resident++;
            } else if (GeomBuf* ib = CachedIndices(ia, dr.count, index_size, strip)) {
                ctx_->IASetIndexBuffer(ib->buf.Get(), ib->index_format, 0);
                index_count = dr.count;
                min_v = ib->min_v;
                max_v = ib->max_v;
            }
        }
        if (index_count)
            ctx_->IASetPrimitiveTopology(topo);
    }
    if (index_count) {
        if (max_v - min_v > 4000000) {
            draws_skipped++, skip_reason_[3]++;
            return;
        }
    } else {
        if (dr.indexed) {
            const u32 fmt = m.index_format;
            index_size = fmt == 0 ? 1 : fmt == 1 ? 2 : 4;
            u64 ia = m.index_addr + u64(dr.first) * index_size;
            if (index_size == 4 && !cull_src_.empty()) // a culling list: its source indices (as above)
                if (auto cs = cull_src_.find(ia); cs != cull_src_.end() && cs->second.count == dr.count) {
                    ia = cs->second.addr;
                    index_size = cs->second.index_size;
                }
            std::vector<u8> raw(size_t(dr.count) * index_size);
            SyncGuest(ia, raw.size(), 4);
            g.ReadGpu(ia, raw.data(), raw.size());
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
        index_count = u32(indices.size());
    }
    g_prof_ib_count += index_count;

    // ---- driver constants + vertex buffers
    Prof(kProfVertex);
    DriverConstants drv{};
    drv.viewport[0] = flip;
    drv.viewport[1] = sz;
    drv.viewport[2] = tz;
    drv.viewport[3] = fs;
    for (u32 i = 0; i < 32; i++)
        drv.attrib[i][0] = m.attrib[i];
    // ---- vertex input: Maxwell attribute formats -> D3D11 input layout
    if (g_seen.layout_vs != vs->hash || g_seen.layout_serial != m.vertex_format_serial) {
        static thread_local std::vector<D3D11_INPUT_ELEMENT_DESC> elems;
        elems.clear();
        bool used_buf[16]{};
        bool need_default = false;
        for (u32 i = 0; i < 32; i++) {
            if (!(vs->info.attrib_in_mask & (1u << i)))
                continue;
            const u32 f = m.attrib[i];
            const u32 b = f & 0x1F;
            const bool constant = f & 0x40;
            const DXGI_FORMAT fmt = AttribFormat(f);
            const bool stream_on = b < 16 && m.streams[b].enabled;
            D3D11_INPUT_ELEMENT_DESC e{"ATTR", i, fmt, b, (f >> 7) & 0x3FFF, D3D11_INPUT_PER_VERTEX_DATA, 0};
            if (constant || fmt == DXGI_FORMAT_UNKNOWN || !stream_on) {
                e = {"ATTR", i, DXGI_FORMAT_R32G32B32A32_FLOAT, 31, 0, D3D11_INPUT_PER_VERTEX_DATA, 0};
                need_default = true;
            } else {
                const u32 divisor = m.streams[b].divisor;
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
        g_seen.layout_vs = vs->hash;
        g_seen.layout_serial = m.vertex_format_serial;
        g_seen.layout = layout;
        std::memcpy(g_seen.used_buf, used_buf, sizeof(used_buf));
        g_seen.need_default = need_default;
    }
    ID3D11InputLayout* const layout = g_seen.layout;
    const bool* const used_buf = g_seen.used_buf;
    const bool need_default = g_seen.need_default;
    if (pipe_.layout != layout) {
        pipe_.layout = layout;
        ctx_->IASetInputLayout(layout);
    }
    ID3D11Buffer* vbufs[32]{};
    UINT strides[32]{}, offsets[32]{};
    s32 base_vertex = 0;
    // Per-vertex buffers come from the geometry cache as whole guest buffers, indexed with absolute vertex numbers;
    // if any of them is not cached, all are uploaded per draw from the drawn vertex range instead.
    GeomBuf* cached_vb[16]{};
    GpuBuf* resident_vb[16]{};
    bool cache_vbs = trace_draws_ <= 0;
    for (u32 b = 0; b < 16 && cache_vbs; b++) {
        if (!used_buf[b] || m.streams[b].divisor)
            continue;
        const u64 addr = m.streams[b].addr;
        const u64 limit = m.streams[b].limit;
        if (limit < addr || limit + 1 - addr > (32u << 20))
            cache_vbs = false;
        else if ((resident_vb[b] = ResidentBuffer(addr, 16))) // compute output (skinning, particles): drawn from the GPU copy
            g_prof_resident++;
        else if (!(cached_vb[b] = CachedVertices(addr, u32(limit + 1 - addr))))
            cache_vbs = false;
    }
    for (u32 b = 0; b < 16; b++) {
        if (!used_buf[b])
            continue;
        const u32 stride = m.streams[b].stride;
        const u32 divisor = m.streams[b].divisor;
        const u64 addr = m.streams[b].addr;
        const u64 limit = m.streams[b].limit;
        if (cache_vbs && !divisor) {
            if (resident_vb[b]) {
                vbufs[b] = resident_vb[b]->buf.Get();
                offsets[b] = u32(addr - resident_vb[b]->addr);
            } else
                vbufs[b] = cached_vb[b]->buf.Get();
            strides[b] = stride;
            continue;
        }
        u64 begin, end;
        if (divisor) {
            begin = addr + u64(stride) * (dr.instance / divisor);
            end = std::min<u64>(limit + 1, begin + stride + 64);
        } else {
            begin = addr + u64(min_v) * stride;
            end = std::min<u64>(limit + 1, addr + u64(max_v + 1) * stride + 64);
            base_vertex = -s32(min_v);
            if (whole_vb) {
                begin = addr;
                end = limit + 1;
                base_vertex = 0;
            }
        }
        if (end <= begin)
            end = begin + 16;
        const u32 size = u32(std::min<u64>(end - begin, 64ull << 20));
        if (GpuBuf* gb = ResidentBuffer(begin, size)) { // compute output: drawn straight from the resident copy
            vbufs[b] = gb->buf.Get();
            offsets[b] = u32(begin - gb->addr);
            strides[b] = stride;
            g_prof_resident++;
            continue;
        }
        static thread_local std::vector<u8> tmp;
        tmp.assign(size_t(size) + 64, 0);
        SyncGuest(begin, size, 5);
        g.ReadGpu(begin, tmp.data(), size);
        vbufs[b] = dc.Vertex(b, tmp.data(), u32(tmp.size()));
        g_prof_vb_bytes += size;
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
    { // only the slots that changed
        u32 lo = 32, hi = 0;
        for (u32 i = 0; i < 32; i++)
            if (pipe_.vb[i] != vbufs[i] || pipe_.vb_stride[i] != strides[i] || pipe_.vb_offset[i] != offsets[i]) {
                lo = std::min(lo, i);
                hi = i + 1;
            }
        if (lo < hi) {
            ctx_->IASetVertexBuffers(lo, hi - lo, vbufs + lo, strides + lo, offsets + lo);
            std::memcpy(pipe_.vb, vbufs, sizeof(vbufs));
            std::memcpy(pipe_.vb_stride, strides, sizeof(strides));
            std::memcpy(pipe_.vb_offset, offsets, sizeof(offsets));
        }
    }
    drv.misc[0] = dr.instance; // added to SV_InstanceID (always 0: each Maxwell draw is one instance)
    drv.misc[1] = m.alpha_func;
    drv.misc[2] = m.alpha_ref;
    drv.misc[3] = m.alpha_test;

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
                const u32 f = m.attrib[i];
                attrs += fmt::format(" {}:b{}+{}/s{:x}t{}{}", i, f & 0x1F, (f >> 7) & 0x3FFF, (f >> 21) & 0x3F, (f >> 27) & 7,
                                     (f & 0x40) ? "c" : "");
            }
        std::string cbs;
        for (u32 i = 0; i < 18; i++)
            if (vs->info.cbuf_mask & (1u << i))
                cbs += fmt::format(" c{}={:#x}@{:#x}", i, m.cb[0][i].size, m.cb[0][i].address);
        std::string progs;
        for (u32 pidx = 0; pidx < kNumGraphicsStages; pidx++)
            progs += fmt::format(" s{}={:#x}", pidx, m.program[pidx]);
        LOG_INFO("Renderer", "TRACE attrs{} | {} | vs cbufs{} | progs{} topo {} | base vertex {:#x} base instance {:#x} idxfmt {} first {}",
                 attrs, trace_vb_, cbs, progs, dr.topology, m.base_vertex, m.base_instance, m.index_format, dr.first);
        // CPU check of the first triangle: attribute-5 positions through bone 0 (c3[14..16]) and c3[10..13].
        if ((vs->info.cbuf_mask & 8) && trace_vb_stride_ && indices.size() >= 3) {
            float c[20][4]{};
            g.ReadGpu(m.cb[0][3].address, c, std::min<size_t>(sizeof(c), m.cb[0][3].size));
            const u32 f5 = m.attrib[5], off5 = (f5 >> 7) & 0x3FFF;
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
                g.ReadGpu(m.cb[4][cb].address, rows, std::min<size_t>(sizeof(rows), m.cb[4][cb].size));
                const u32 n = std::min<u32>(176, m.cb[4][cb].size / 16);
                for (u32 base = 0; base < n; base += 16) {
                    std::string r;
                    for (u32 k = base; k < std::min(n, base + 16); k++)
                        r += fmt::format(" [{}]({:g},{:g},{:g},{:g})", k, rows[k][0], rows[k][1], rows[k][2], rows[k][3]);
                    LOG_INFO("Renderer", "TRACE ps c{} size {:#x}:{}", cb, m.cb[4][cb].size, r);
                }
            }
        }
        if (vs->info.cbuf_mask & 8) {
            float rows[20][4]{};
            g.ReadGpu(m.cb[0][3].address, rows, std::min<size_t>(sizeof(rows), m.cb[0][3].size));
            std::string r;
            for (auto& row : rows)
                r += fmt::format(" ({:g},{:g},{:g},{:g})", row[0], row[1], row[2], row[3]);
            LOG_INFO("Renderer", "TRACE c3[0..19]{}", r);
        }
        LOG_INFO("Renderer", "TRACE ib {:#x} first {} count {} indexed {}", m.index_addr, dr.first, dr.count, dr.indexed);
        LOG_INFO("Renderer", "TRACE vp sz {:g} tz {:g} sy {:g} near {:g} far {:g}", sz, tz, sy, m.depth_near, m.depth_far);
        LOG_INFO("Renderer", "TRACE draw vs {:016x} ps {:016x} n {} rts{} blend0 {} cmask {:#x} depth {} ztest {} zwrite {} zfunc {:#x}",
                 vs->hash, ps ? ps->hash : 0, indices.size(), rts, m.blend_enable[0], m.color_mask[0],
                 depth ? fmt::format("{:#x}/{:#x}", depth->gpu_va, depth->format) : "-", m.depth_test,
                 m.depth_write, m.depth_func);
        LOG_INFO("Renderer", "TRACE stencil en {} func {:#x} ref {:#x} fmask {:#x} wmask {:#x} ops {:#x}/{:#x}/{:#x} back func {:#x} ops {:#x}/{:#x}/{:#x} cull {} {:#x} front ccw {}",
                 m.stencil_test, m.stencil_front.func, m.stencil_ref, m.stencil_read_mask, m.stencil_write_mask,
                 m.stencil_front.op_fail, m.stencil_front.op_zfail, m.stencil_front.op_zpass, m.stencil_back.func,
                 m.stencil_back.op_fail, m.stencil_back.op_zfail, m.stencil_back.op_zpass, m.cull_enable, m.cull_face, m.front_ccw);
    }
    // ---- per stage resources
    Prof(kProfResources);
    auto bind_stage = [&](HostShader* s, u32 stage_index, bool is_vs) {
        // constant buffers
        Prof(kProfCbufs);
        ID3D11Buffer* cbs[13]{};
        ID3D11ShaderResourceView* raws[13]{}; // dynamically indexed constant buffers as raw views (t32+)
        for (u32 i = 0; i < 13; i++) {
            if (!(s->info.cbuf_mask & (1u << i)))
                continue;
            const auto& b = m.cb[stage_index][i];
            if (!b.enabled || !b.size)
                continue;
            if (LearnEnabled() && LearnOnce(b.address ^ (u64(stage_index * 32 + i) << 56) ^ 0x5A5A))
                LOG_INFO("Learn", "cbuf stage {} slot {} address {:#x} size {:#x}", stage_index, i, b.address, b.size);
            static thread_local std::vector<u8> tmp;
            // Upload only what the shader can read: its highest static row, or everything for dynamic indexing.
            const bool dyn = s->info.cbuf_dyn_mask & (1u << i);
            const u32 need = dyn ? 65536u : s->info.cbuf_rows[i] * 16u;
            const u32 size = u32(common::AlignUp(std::max<u32>(std::min<u32>({b.size, 65536u, need}), 16u), 16));
            SyncGuest(b.address, size, 6);
            // Same bytes as this slot's last upload: keep the buffer (most draws share material/scene constants).
            CbShadow& sh = cb_shadow_[stage_index * 32 + i];
            const u8* src = g.GpuPtr(b.address, size);
            if (src && !trace_draw_ && sh.addr == b.address && sh.data.size() == size && (!dyn || sh.raw) &&
                std::memcmp(src, sh.data.data(), size) == 0) {
                cbs[i] = sh.buf;
                if (dyn)
                    raws[i] = sh.raw;
                continue;
            }
            // Contiguous guest memory is uploaded straight from its host copy (no staging copy per upload).
            const u8* data = src;
            if (!data || trace_draw_) {
                tmp.resize(size);
                g.ReadGpu(b.address, tmp.data(), size);
                data = tmp.data();
            }
            cbs[i] = dc.Constant(stage_index * 32 + i, data, size);
            sh.addr = b.address;
            sh.data.assign(data, data + size);
            sh.buf = cbs[i];
            sh.raw = nullptr;
            g_prof_cb_bytes += size;
            if (trace_draw_ && !is_vs && size >= 13 * 16) {
                const float* f = reinterpret_cast<const float*>(data);
                LOG_INFO("Renderer", "TRACE upload ps c{} {:#x}@{:#x} size {:#x} dyn {} rows10-12.w {:g} {:g} {:g}", i, b.size, b.address, size,
                         (s->info.cbuf_dyn_mask >> i) & 1, f[10 * 4 + 3], f[11 * 4 + 3], f[12 * 4 + 3]);
            }
            if (dyn) {
                ID3D11ShaderResourceView* raw = dc.RawBuffer(stage_index * 32 + i, data, size);
                sh.raw = raw;
                raws[i] = raw;
            }
        }
        // textures
        Prof(kProfTextures);
        ID3D11ShaderResourceView* srvs[32]{};
        ID3D11SamplerState* smps[16]{};
        read_stage_textures(s, stage_index);
        const u64 tsc_base = m.tsc_base;
        for (auto& t : s->info.textures) {
            if (t.slot >= 32)
                continue;
            const StageTexture& st = stage_textures[is_vs ? 0 : 1][t.slot];
            const u32 handle = st.handle;
            const TicEntry& tic = st.tic;
            if (LearnEnabled() && LearnOnce((u64(handle) << 16) ^ (u64(stage_index) << 8) ^ t.cbuf_offset ^ (1ull << 63)))
                LOG_INFO("Learn", "texture stage {} cbuf {} offset {:#x} handle {:#x}", stage_index, kTextureHandleCb,
                         t.cbuf_offset, handle);
            const u32 tic_idx = handle & 0xFFFFF, tsc_idx = (handle >> 20) & 0xFFF;
            u32 tsc[8];
            g.ReadGpu(tsc_base + u64(tsc_idx) * 32, tsc, 32);
            u32 swz = 0xB1A;
            const u64 tg0 = g_profile ? ProfNow() : 0;
            srvs[t.slot] = textures_->Get(tic, t, swz);
            if (g_profile)
                g_prof_tex_get_ns += ProfNow() - tg0, g_prof_tex_calls++;
            if (frame_counter_ - recent_wanted_ <= 120)
                NoteTexture(srvs[t.slot], tic.Address(), tic.Width(), tic.Height(), tic.Format(),
                            FindSurfaceByGpu(tic.Address()) != nullptr);
            if (trace_draw_)
                LOG_INFO("Renderer", "    {} tex slot {} handle {:#x} tic {} addr {:#x} fmt {:#x} type {} {}x{} w0 {:#x} swz {:#x} srgb {} mips {} {}", is_vs ? "VS" : "PS",
                         t.slot, handle, tic_idx, tic.Address(), tic.Format(), tic.Type(), tic.Width(), tic.Height(), tic.w[0], swz, tic.Srgb(), tic.MaxMip(),
                         FindSurfaceByGpu(tic.Address()) ? "(render target)" : "");
            drv.swizzle[t.slot][0] = swz;
            drv.swizzle[t.slot][1] = TextureScale(tic.Address(), t.dim == shader::TexDim::Tex2D && !t.array);
            const u64 ts0 = g_profile ? ProfNow() : 0;
            if (t.sampler < 16)
                smps[t.sampler] = textures_->GetSampler(tsc, t.shadow);
            if (g_profile)
                g_prof_tex_samp_ns += ProfNow() - ts0;
        }
        StageBinds& sb = binds_[is_vs ? 0 : 1];
        if (is_vs) {
            BindRange(sb.cb, cbs, 0, 13, [&](u32 s, u32 n, ID3D11Buffer* const* p) { ctx_->VSSetConstantBuffers(s, n, p); });
            BindRange(sb.srv, srvs, 0, 32, [&](u32 s, u32 n, ID3D11ShaderResourceView* const* p) { ctx_->VSSetShaderResources(s, n, p); });
            BindRange(sb.srv, raws, 32, 13, [&](u32 s, u32 n, ID3D11ShaderResourceView* const* p) { ctx_->VSSetShaderResources(s, n, p); });
            BindRange(sb.smp, smps, 0, 16, [&](u32 s, u32 n, ID3D11SamplerState* const* p) { ctx_->VSSetSamplers(s, n, p); });
        } else {
            BindRange(sb.cb, cbs, 0, 13, [&](u32 s, u32 n, ID3D11Buffer* const* p) { ctx_->PSSetConstantBuffers(s, n, p); });
            BindRange(sb.srv, srvs, 0, 32, [&](u32 s, u32 n, ID3D11ShaderResourceView* const* p) { ctx_->PSSetShaderResources(s, n, p); });
            BindRange(sb.srv, raws, 32, 13, [&](u32 s, u32 n, ID3D11ShaderResourceView* const* p) { ctx_->PSSetShaderResources(s, n, p); });
            BindRange(sb.smp, smps, 0, 16, [&](u32 s, u32 n, ID3D11SamplerState* const* p) { ctx_->PSSetSamplers(s, n, p); });
        }
    };
    // Unbind surfaces that may also be used as textures this draw (D3D forbids read/write hazards).
    bind_stage(vs, 0, true);
    if (ps)
        bind_stage(ps, 4, false);
    Prof(kProfResources);

    // Driver constants rarely change between consecutive draws: re-upload only when they do.
    static DriverConstants last_drv;
    static ID3D11Buffer* last_drv_cb = nullptr;
    if (!last_drv_cb || std::memcmp(&last_drv, &drv, sizeof(drv)) != 0) {
        last_drv = drv;
        last_drv_cb = dc.Constant(1000, &drv, sizeof(drv));
    }
    ID3D11Buffer* drv_cb = last_drv_cb;
    BindRange(binds_[0].cb, &drv_cb, 13, 1, [&](u32 s, u32 n, ID3D11Buffer* const* p) { ctx_->VSSetConstantBuffers(s, n, p); });
    BindRange(binds_[1].cb, &drv_cb, 13, 1, [&](u32 s, u32 n, ID3D11Buffer* const* p) { ctx_->PSSetConstantBuffers(s, n, p); });

    if (pipe_.vs != vs->vs.Get()) {
        pipe_.vs = vs->vs.Get();
        ctx_->VSSetShader(pipe_.vs, nullptr, 0);
    }
    if (ID3D11PixelShader* p = ps ? ps->ps.Get() : nullptr; pipe_.ps != p) {
        pipe_.ps = p;
        ctx_->PSSetShader(p, nullptr, 0);
    }
    if (debug_mode_ >= 1) { // MUA3_DEBUG_PS=1: solid magenta; 2: real shaders; both without blend/depth
        if (debug_mode_ == 1)
            ctx_->PSSetShader(debug_ps_.Get(), nullptr, 0);
        ctx_->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
        ctx_->OMSetDepthStencilState(nullptr, 0);
    }
    if (pipe_.gs) {
        pipe_.gs = nullptr;
        ctx_->GSSetShader(nullptr, nullptr, 0);
    }

    static int traced = 0;
    if (traced < 12) {
        traced++;
        LOG_INFO("Renderer", "DRAW vs {:016x} ps {:016x} topo {} idx {} n {} [{}..{}] rt0 {:#x} fmt {:#x} {}x{} vp ({},{} {}x{}) flip {} sz {} tz {}",
                 vs->hash, ps ? ps->hash : 0, dr.topology, dr.indexed, indices.size(), min_v, max_v,
                 colors[0] ? colors[0]->gpu_va : 0, colors[0] ? colors[0]->format : 0, colors[0] ? colors[0]->width : 0,
                 colors[0] ? colors[0]->height : 0, vp.TopLeftX, vp.TopLeftY, vp.Width, vp.Height, flip, sz, tz);
        LOG_INFO("Renderer", "     scissor en {} ({},{})-({},{}) cmask0 {:#x} blend0 {} rtc {:#x} depth {} attr0 {:#x} attr1 {:#x} vb0 {:#x} {:#x}",
                 m.scissor_enable, sr.left, sr.top, sr.right, sr.bottom, m.color_mask[0], m.blend_enable[0], count,
                 depth ? 1 : 0, m.attrib[0], m.attrib[1], m.streams[0].stride,
                 m.streams[0].addr);
    }
    ComPtr<ID3D11Query> pick_query;
    if (picking) {
        D3D11_QUERY_DESC qd{D3D11_QUERY_OCCLUSION, 0};
        dev_->CreateQuery(&qd, &pick_query);
        ctx_->Begin(pick_query.Get());
    }
    Prof(kProfSubmit);
    if (indirect_quads)
        ctx_->DrawIndexedInstancedIndirect(quad_args_buf_.Get(), 0);
    else if (direct && indirect)
        ctx_->DrawInstancedIndirect(indirect->buf.Get(), u32(dr.indirect_args - indirect->addr));
    else if (direct)
        ctx_->DrawInstanced(index_count, 1, u32(s32(dr.first) + base_vertex), 0);
    else
        ctx_->DrawIndexedInstanced(index_count, 1, 0, base_vertex, 0);
    if (pick_query) {
        ctx_->End(pick_query.Get());
        UINT64 samples = 0;
        while (ctx_->GetData(pick_query.Get(), &samples, sizeof(samples), 0) == S_FALSE) {
        }
        if (samples || counting)
            LOG_INFO("Renderer", "{} draw vs {:016x} ps {:016x} rt0 {:#x} n {} blend {} zwrite {} samples {}",
                     counting ? "OCCL" : "PICK", vs->hash, ps ? ps->hash : 0, colors[0] ? colors[0]->gpu_va : 0,
                     indices.size(), m.blend_enable[0], m.depth_write, samples);
    }
    draws_ok++;
    for (u32 i = 0; i < count; i++)
        if (colors[i] && (m.color_mask[i] | m.color_mask[0]))
            colors[i]->write_serial = ++surface_writes_;
    if (depth && (m.depth_write || m.stencil_test))
        depth->write_serial = ++surface_writes_;

    // Texture bindings stay in place: binding a surface as an output later unbinds it (and resets the cache).
}

// A frame far longer than usual (a stutter) logs what this thread did during it: waits for shader compiles, programs
// and integer variants seen for the first time, driver shader objects created, texture uploads, and (MUA3_PROFILE)
// the draw sections. Little of that in a long frame means the time went elsewhere (guest code, NVN layer).
static u64 g_hitch_prof[kProfCount];
constexpr s64 kShaderWaitBudgetNs = 30'000'000;
void D3D11Renderer::ReportHitch() {
    static auto last = std::chrono::steady_clock::now();
    static u64 uploads = 0, realize_ns = 0, realized = 0;
    static u32 programs = 0, variants = 0;
    const auto now = std::chrono::steady_clock::now();
    const double ms = double((now - last).count()) / 1e6;
    last = now;
    ShaderCache& sc = *shaders_;
    if (ms > 50 && frame_counter_ > 300) {
        std::string prof;
        if (g_profile)
            for (int i = 0; i < kProfCount; i++)
                prof += fmt::format(" {} {:.1f}", kProfNames[i], double(g_prof_ns[i] - g_hitch_prof[i]) / 1e6);
        LOG_WARN("Hitch", "frame {:.1f} ms: shader wait {:.1f} ms, new programs {} variants {}, driver shaders {} "
                          "({:.1f} ms), texture uploads {}, compiles pending {}{}{}",
                 ms, double(kShaderWaitBudgetNs - shader_wait_ns_) / 1e6, sc.new_programs - programs, sc.new_variants - variants,
                 sc.realized - realized, double(sc.realize_ns - realize_ns) / 1e6, textures_->uploads - uploads,
                 sc.pending.load(), prof.empty() ? "" : " | ms:", prof);
    }
    uploads = textures_->uploads;
    realize_ns = sc.realize_ns;
    realized = sc.realized;
    programs = sc.new_programs;
    variants = sc.new_variants;
    std::memcpy(g_hitch_prof, g_prof_ns, sizeof(g_prof_ns));
}

void D3D11Renderer::Present(const PresentInfo& p) {
    std::lock_guard ctx_lock(ContextMutex());
    frame_counter_++;
    shaders_->Tick();
    ReportHitch();
    // D3D11 allows 4,096 live state objects of each kind and gets very slow near that limit: draws whose depth bias
    // changes every frame make a new rasterizer state each time (a 3 s freeze and then failed creations, in an
    // ultimate move). Past 1,024 the caches start over; objects still bound stay alive in the context until replaced.
    if (g_dc && (g_dc->blend.size() > 1024 || g_dc->depth.size() > 1024 || g_dc->raster.size() > 1024)) {
        LOG_INFO("Renderer", "state object caches flushed ({} blend, {} depth-stencil, {} rasterizer)", g_dc->blend.size(),
                 g_dc->depth.size(), g_dc->raster.size());
        g_dc->blend.clear();
        g_dc->depth.clear();
        g_dc->raster.clear();
        g_seen.blend = g_seen.depth = g_seen.raster = ~0u; // recomputed (and recreated) at the next draw
        g_seen.blend_obj = nullptr;
        g_seen.depth_obj = nullptr;
        g_seen.raster_obj = nullptr;
        InvalidatePipe();
    }
    common::g_last_present_ns.store(std::chrono::steady_clock::now().time_since_epoch().count(),
                                    std::memory_order_relaxed);
    // Per frame, draws may wait this long for shaders the game just asked for: a bytecode cache hit makes it, a real
    // compile (FXC, tens to hundreds of ms) leaves the draw out for a frame or two instead of freezing the game.
    shader_wait_ns_ = kShaderWaitBudgetNs;
    if (g_profile && frame_counter_ % 150 == 0) {
        std::string line;
        for (int i = 0; i < kProfCount; i++)
            line += fmt::format(" {} {:.2f}", kProfNames[i], g_prof_ns[i] / 150e6);
        static u64 last_reuploads = 0;
        LOG_INFO("Profile", "per frame (ms):{} | draws {} vb {:.1f} MB cb {:.1f} MB indices {}k | tex reuploads {:.1f} geom {}/{}",
                 line, g_prof_draws / 150, g_prof_vb_bytes / 150 / 1048576.0, g_prof_cb_bytes / 150 / 1048576.0,
                 g_prof_ib_count / 150 / 1000, (textures_->reuploads - last_reuploads) / 150.0, index_cache_.size(),
                 vertex_cache_.size());
        last_reuploads = textures_->reuploads;
        LOG_INFO("Profile", "  copy-backs: {:.1f} queued/frame ({:.2f} MB), {:.2f} blocking waits/frame costing {:.2f} ms, {:.1f} resident binds/frame",
                 g_prof_rb_queued / 150.0, g_prof_rb_bytes / 150 / 1048576.0, g_prof_rb_waits / 150.0, g_prof_rb_wait_ns / 150e6,
                 g_prof_resident / 150.0);
        g_prof_resident = 0;
        LOG_INFO("Profile", "  copy-back pages: {:.0f} compared/frame, {:.0f} changed; {:.1f} polls/frame found none ready",
                 g_prof_rb_pages / 150.0, g_prof_rb_changed_pages / 150.0, g_prof_rb_not_ready / 150.0);
        g_prof_rb_pages = g_prof_rb_changed_pages = g_prof_rb_not_ready = 0;
        static u64 last_tex_track = 0;
        LOG_INFO("Profile", "  write tracking re-armed per frame: textures {:.0f} KB, compute {:.0f} KB, index/vertex {:.0f} KB",
                 (textures_->tracked_bytes - last_tex_track) / 150.0 / 1024, g_prof_track_bytes[1] / 150.0 / 1024,
                 g_prof_track_bytes[2] / 150.0 / 1024);
        last_tex_track = textures_->tracked_bytes;
        g_prof_track_bytes[1] = g_prof_track_bytes[2] = 0;
        LOG_INFO("Profile", "  waits by consumer: compute {} texture {} cull-index {} cull-counter {} index {} vertex {} cbuf {} geometry {}",
                 g_prof_rb_site[0], g_prof_rb_site[1], g_prof_rb_site[2], g_prof_rb_site[3], g_prof_rb_site[4], g_prof_rb_site[5],
                 g_prof_rb_site[6], g_prof_rb_site[7]);
        LOG_INFO("Profile", "  waits inside compute: overlap {} stale {} grow-fallback {} sync-region {}", g_prof_rb_site[8], g_prof_rb_site[9],
                 g_prof_rb_site[10], g_prof_rb_site[11]);
        std::memset(g_prof_rb_site, 0, sizeof(g_prof_rb_site));
        g_prof_rb_queued = g_prof_rb_bytes = g_prof_rb_waits = g_prof_rb_wait_ns = 0;
        static u64 last_uploads = 0, last_unchanged = 0;
        LOG_INFO("Profile", "  textures: {} lookups/frame, Get {:.2f} ms, samplers {:.2f} ms, uploads {:.1f}/frame, "
                            "CPU-written but unchanged {:.1f}/frame",
                 g_prof_tex_calls / 150, g_prof_tex_get_ns / 150e6, g_prof_tex_samp_ns / 150e6,
                 (textures_->uploads - last_uploads) / 150.0, (textures_->unchanged_rewrites - last_unchanged) / 150.0);
        last_uploads = textures_->uploads;
        last_unchanged = textures_->unchanged_rewrites;
        g_prof_tex_get_ns = g_prof_tex_samp_ns = g_prof_tex_calls = 0;
        std::memset(g_prof_ns, 0, sizeof(g_prof_ns));
        std::memset(g_hitch_prof, 0, sizeof(g_hitch_prof));
        g_prof_vb_bytes = g_prof_cb_bytes = g_prof_ib_count = g_prof_draws = 0;
    }
    // Copy this frame's compute results back (applied to guest memory once the copies land, a frame or so later;
    // host-side reads of that memory force it earlier via SyncGuest).
    if (gpu_dirty_any_) {
        for (auto& b : gbufs_)
            if (b->gpu_dirty)
                QueueReadback(*b);
        gpu_dirty_any_ = false;
    }
    PollReadbacks(false);
    std::erase_if(gbufs_, [&](auto& b) { return !b->in_flight && frame_counter_ - b->last_use > 600; });
    active_stale_ = true;
    for (auto* cache : {&index_cache_, &vertex_cache_})
        std::erase_if(*cache, [&](auto& kv) {
            const u64 idle = frame_counter_ - kv.second.last_use;
            return idle > 600 || (!kv.second.buf && !kv.second.streaming && idle > 2);
        });
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
    if (const int aa = core::Sys().Cfg().anti_aliasing; aa != applied_aa_)
        ApplyAntiAliasing(aa);
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
            const D3D11_BOX box{r.x * sf->scale, r.y * sf->scale, 0, r.x * sf->scale + 1, r.y * sf->scale + 1, 1};
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
        // video memory in use by this process (the VRAM budget work)
        if (frame_counter_ % 300 == 1) {
            Microsoft::WRL::ComPtr<IDXGIDevice> dd;
            Microsoft::WRL::ComPtr<IDXGIAdapter> ad;
            Microsoft::WRL::ComPtr<IDXGIAdapter3> ad3;
            DXGI_QUERY_VIDEO_MEMORY_INFO vm{};
            if (SUCCEEDED(dev_->QueryInterface(IID_PPV_ARGS(&dd))) && SUCCEEDED(dd->GetAdapter(&ad)) && SUCCEEDED(ad.As(&ad3)) &&
                SUCCEEDED(ad3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &vm))) {
                u64 surf = 0;
                for (auto& [k, sf] : surfaces_)
                    surf += u64(sf->width * sf->scale) * (sf->height * sf->scale) * 4;
                DXGI_QUERY_VIDEO_MEMORY_INFO sys{};
                ad3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &sys);
                LOG_INFO("Renderer", "VRAM in use {} MB (budget {} MB), GPU system memory {} MB; render targets ~{} MB in {} surfaces",
                         vm.CurrentUsage >> 20, vm.Budget >> 20, sys.CurrentUsage >> 20, surf >> 20, surfaces_.size());
            }
        }
        if (!s)
            for (auto& [k, sf] : surfaces_)
                LOG_INFO("Renderer", "  surface gpu {:#x} cpu {:#x} {}x{} fmt {:#x}{}", sf->gpu_va, sf->cpu_va, sf->width,
                         sf->height, sf->format, sf->depth ? " depth" : "");
    }
    std::lock_guard lk(frame_mutex_);
    if (s) {
        D3D11_TEXTURE2D_DESC desc;
        s->tex->GetDesc(&desc);
        // A supersampled frame is shown at the render scale (resolved below).
        const u32 ssaa = s->scale > base_scale_ && s->scale % base_scale_ == 0 ? s->scale / base_scale_ : 1;
        const u32 out_w = s->width * s->scale / ssaa, out_h = s->height * s->scale / ssaa;
        if (!frame_tex_ || frame_w_ != out_w || frame_h_ != out_h || frame_dxgi_ != s->dxgi) {
            frame_tex_.Reset();
            frame_srv_.Reset();
            frame_rtv_.Reset();
            D3D11_TEXTURE2D_DESC td = desc;
            td.Width = out_w;
            td.Height = out_h;
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET; // (target of the AA pass)
            dev_->CreateTexture2D(&td, nullptr, &frame_tex_);
            D3D11_RENDER_TARGET_VIEW_DESC rv{};
            rv.Format = s->dxgi;
            rv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
            dev_->CreateRenderTargetView(frame_tex_.Get(), &rv, &frame_rtv_);
            frame_dxgi_ = s->dxgi;
            D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
            sv.Format = s->dxgi == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : s->dxgi;
            sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            sv.Texture2D.MipLevels = 1;
            dev_->CreateShaderResourceView(frame_tex_.Get(), &sv, &frame_srv_);
            frame_w_ = out_w;
            frame_h_ = out_h;
        }
        // Anti-aliasing (mua3.ini / the in-game Graphics menu, read live): FXAA into the frame shown; off, a copy.
        const bool srgb = s->dxgi == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || s->dxgi == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        if (ssaa > 1) {
            if (post_aa_.Downsample(dev_, ctx_, s->srv.Get(), frame_rtv_.Get(), frame_w_, frame_h_, srgb, ssaa))
                ContextEpoch()++; // (the draw state caches forget what the pass bound)
        } else if (applied_aa_ == 1 &&
                   post_aa_.Fxaa(dev_, ctx_, s->srv.Get(), frame_rtv_.Get(), frame_w_, frame_h_, srgb))
            ContextEpoch()++;
        else
            ctx_->CopyResource(frame_tex_.Get(), s->tex.Get());
        SetEvent(static_cast<HANDLE>(new_frame_event_));
    }
    ctx_->Flush();
}

// Debug: writes a colour surface to a .bmp (RGBA8/BGRA8/RGBA16F/R11G11B10 supported).
void D3D11Renderer::DumpSurface(Surface* s, const fs::path& path) {
    D3D11_TEXTURE2D_DESC td;
    s->tex->GetDesc(&td);
    const u32 dump_w = td.Width, dump_h = td.Height; // host size (resolution scaling)
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
    std::vector<u8> bgr(size_t(dump_w) * dump_h * 3);
    auto f16 = [](u16 h) {
        const u32 e = (h >> 10) & 0x1F, mnt = h & 0x3FF;
        float v = e == 0 ? mnt / 1024.0f / 16384.0f : std::ldexp(1.0f + mnt / 1024.0f, int(e) - 15);
        return (h & 0x8000) ? -v : v;
    };
    for (u32 y = 0; y < dump_h; y++) {
        const u8* row = static_cast<const u8*>(m.pData) + size_t(y) * m.RowPitch;
        for (u32 x = 0; x < dump_w; x++) {
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
            u8* o = &bgr[(size_t(dump_h - 1 - y) * dump_w + x) * 3];
            o[0] = u8(std::clamp(b, 0.f, 1.f) * 255), o[1] = u8(std::clamp(g, 0.f, 1.f) * 255),
            o[2] = u8(std::clamp(r, 0.f, 1.f) * 255);
        }
    }
    ctx_->Unmap(st.Get(), 0);
    const u32 row_size = (dump_w * 3 + 3) & ~3u;
    std::vector<u8> file(54 + size_t(row_size) * dump_h);
    auto put32 = [&](size_t o, u32 v) { std::memcpy(&file[o], &v, 4); };
    file[0] = 'B', file[1] = 'M';
    put32(2, u32(file.size())), put32(10, 54), put32(14, 40), put32(18, dump_w), put32(22, dump_h);
    file[26] = 1, file[28] = 24;
    for (u32 y = 0; y < dump_h; y++)
        std::memcpy(&file[54 + size_t(y) * row_size], &bgr[size_t(y) * dump_w * 3], size_t(dump_w) * 3);
    common::WriteFile(path, file);
}

void D3D11Renderer::NoteTexture(ID3D11ShaderResourceView* srv, u64 addr, u32 w, u32 h, u32 fmt, bool rt) {
    if (!srv || frame_counter_ - recent_wanted_ > 120) // only while the overlay's texture viewer is reading them
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

void D3D11Renderer::ThrottleFrames() {
    constexpr u32 kRing = kFramesInFlight + 1;
    {
        std::lock_guard lk(ContextMutex());
        auto& q = frame_queries_[frame_query_count_ % kRing];
        if (!q) {
            const D3D11_QUERY_DESC d{D3D11_QUERY_EVENT, 0};
            dev_->CreateQuery(&d, &q);
        }
        if (q)
            ctx_->End(q.Get());
    }
    frame_query_count_++;
    if (frame_query_count_ <= kFramesInFlight)
        return;
    // The query ended kFramesInFlight presents ago must have completed before this frame's successor is recorded.
    ID3D11Query* old = frame_queries_[(frame_query_count_ - 1 - kFramesInFlight) % kRing].Get();
    if (!old)
        return;
    const u64 t0 = ProfNow();
    for (u32 spin = 0;; spin++) {
        BOOL done = FALSE;
        HRESULT hr;
        {
            std::lock_guard lk(ContextMutex()); // never held while sleeping: other threads need the context
            hr = ctx_->GetData(old, &done, sizeof(done), 0);
        }
        if (hr != S_FALSE)
            break; // S_OK = done; a failure (device removed) must not hang the GPU thread
        if (spin < 64)
            std::this_thread::yield();
        else
            std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    throttle_wait_ns_ += ProfNow() - t0;
    if (++throttle_frames_ % 600 == 0) {
        LOG_INFO("Renderer", "frame throttle: {:.2f} ms/frame waiting for the GPU ({} frames in flight max)",
                 double(throttle_wait_ns_) / 1e6 / 600.0, kFramesInFlight);
        throttle_wait_ns_ = 0;
    }
}

void D3D11Renderer::BindRenderTargets(Surface**, u32, Surface*) {}

} // namespace gpu
