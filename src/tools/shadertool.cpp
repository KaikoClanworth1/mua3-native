#include <cstdio>
// Translates dumped Maxwell shaders (shader_dump/*.vs.bin|*.ps.bin) to HLSL and compiles them with FXC.
//   shadertool <file.bin> [--print]        translate one shader and print the HLSL
//   shadertool <dir>                       translate + compile every dump, report failures/unimplemented ops
#include <d3d11.h>
#include <d3d11shader.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <cmath>
#include <unordered_map>
#include <chrono>
#include <algorithm>
#include <fstream>
#include <map>
#include <set>

#include "gpu/shader/known_programs.h"
#include "loader/formats.h"

#include "gpu/shader/maxwell_isa.h"
#include "gpu/dxbc/dxbc.h"
#include "gpu/shader/harvest.h"
#include "gpu/shader/translator.h"

#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "d3d11.lib")

using namespace gpu::shader;

static UINT g_flags = D3DCOMPILE_OPTIMIZATION_LEVEL1;
static bool CompileHlsl(const std::string& src, const char* target, std::string& err) {
    ID3DBlob *code = nullptr, *errors = nullptr;
    const HRESULT hr = D3DCompile(src.data(), src.size(), nullptr, nullptr, nullptr, "main", target, g_flags, 0, &code,
                                  &errors);
    if (errors) {
        err.assign((const char*)errors->GetBufferPointer(), errors->GetBufferSize());
        errors->Release();
    }
    if (code)
        code->Release();
    return SUCCEEDED(hr);
}

static void Disassemble(const std::vector<u8>& data) {
    for (size_t off = 0x50; off + 8 <= data.size(); off += 8) {
        const u32 pc = u32(off - 0x50);
        if (pc % 32 == 0)
            continue;
        u64 v;
        std::memcpy(&v, &data[off], 8);
        fmt::print("{:06x}: {:016x}  {}\n", pc, v, OpName(Decode(v)));
    }
}

static u64 ComputeKey(const std::vector<u8>& code, const u32 shape[4]) {
    return gpu::Hash64(code.data(), code.size()) ^ (u64(Stage::Compute) << 60) ^
           gpu::Hash64(shape, 16) * 0x9E3779B97F4A7C15ull;
}

static int WriteKnown(const fs::path& dump, const fs::path& game, const fs::path& out_path) {
    // Searched blobs: the main module image, then every file under romfs/SHADER.
    std::vector<std::string> names{"main"};
    std::vector<std::vector<u8>> blobs;
    {
        const auto nso = common::ReadFile(game / "exefs" / "main");
        auto img = loader::LoadNso(nso);
        if (!img) {
            fmt::print("cannot load {}\n", (game / "exefs" / "main").string());
            return 1;
        }
        blobs.push_back(std::move(img->image));
    }
    std::error_code ec;
    for (const auto& e : fs::recursive_directory_iterator(game / "romfs" / "SHADER", ec))
        if (e.is_regular_file()) {
            names.push_back(fs::relative(e.path(), game / "romfs").generic_string());
            blobs.push_back(common::ReadFile(e.path()));
        }
    auto locate = [&](const u8* p, size_t n, u16& file, u32& off) {
        const size_t key = std::min<size_t>(n, 64);
        for (size_t b = 0; b < blobs.size(); b++) {
            const auto& d = blobs[b];
            for (auto it = d.begin();; ++it) {
                it = std::search(it, d.end(), p, p + key);
                if (it == d.end())
                    break;
                const size_t at = size_t(it - d.begin());
                if (at + n <= d.size() && std::memcmp(&d[at], p, n) == 0) {
                    file = u16(b), off = u32(at);
                    return true;
                }
            }
        }
        return false;
    };
    std::vector<KnownProgram> cs;
    size_t total = 0, no_shape = 0, not_found = 0;
    for (const auto& e : fs::directory_iterator(dump, ec)) {
        const std::string name = e.path().filename().string();
        if (name.size() != 23 || !name.ends_with(".cs.bin"))
            continue;
        total++;
        const u64 hash = std::strtoull(name.substr(0, 16).c_str(), nullptr, 16);
        const auto code = common::ReadFile(e.path());
        if (code.size() <= 0x50 + 8)
            continue;
        KnownProgram k{};
        k.hash = hash;
        k.stage = 2;
        k.cb_size = kUnknownCbuf;
        if (!locate(code.data() + 0x50, code.size() - 0x50, k.file, k.offset)) {
            not_found++;
            fmt::print("{:016x}: program not found in the game data\n", hash);
            continue;
        }
        k.size = u32(code.size() - 0x50);
        u32 shape[4]{};
        const auto opt = common::ReadFile(dump / fmt::format("{:016x}.cs.opt", hash));
        bool have = false;
        if (opt.size() >= 16) {
            std::memcpy(shape, opt.data(), 16);
            have = ComputeKey(code, shape) == hash;
            if (have && opt.size() > 16) {
                u16 f;
                if (locate(opt.data() + 16, opt.size() - 16, f, k.cb_offset) && f == 0) {
                    k.cb_size = u32(opt.size() - 16);
                    k.cb_hash = gpu::Hash64(opt.data() + 16, opt.size() - 16);
                }
            } else if (have) {
                k.cb_size = 0;
            }
        }
        // Dumps from before launch options were recorded: the shape is in the key, so search the likely ones.
        for (u32 x = 1; !have && x <= 1024; x *= 2)
            for (u32 y = 1; !have && y <= 32; y *= 2)
                for (u32 z = 1; !have && z <= 4; z *= 2)
                    for (u32 sh = 0; !have && sh <= 48 * 1024; sh += 16) {
                        const u32 s[4] = {x, y, z, sh};
                        if (ComputeKey(code, s) == hash) {
                            std::memcpy(shape, s, 16);
                            have = true;
                        }
                    }
        if (!have) {
            no_shape++;
            fmt::print("{:016x}: launch shape unknown\n", hash);
            continue;
        }
        k.block[0] = shape[0], k.block[1] = shape[1], k.block[2] = shape[2], k.shared_bytes = shape[3];
        cs.push_back(k);
    }
    // Vertex/pixel programs the harvest does not find (it only accepts programs that translate, at offsets with a
    // plausible header), located in the same files.
    size_t gfx_total = 0, gfx_harvested = 0, gfx_added = 0, gfx_missing = 0;
    {
        const fs::path tmp = fs::temp_directory_path() / "mua3_known_harvest";
        fs::remove_all(tmp, ec);
        HarvestPrograms(game / "romfs" / "SHADER", tmp);
        for (const auto& e : fs::directory_iterator(dump, ec)) {
            const std::string name = e.path().filename().string();
            if (name.size() != 23 || !(name.ends_with(".vs.bin") || name.ends_with(".ps.bin")))
                continue;
            gfx_total++;
            if (fs::exists(tmp / name)) {
                gfx_harvested++;
                continue;
            }
            const auto code = common::ReadFile(e.path());
            KnownProgram k{};
            k.hash = std::strtoull(name.substr(0, 16).c_str(), nullptr, 16);
            k.stage = name[17] == 'v' ? 0 : 1;
            k.size = u32(code.size());
            if (code.size() < 0x58 || !locate(code.data(), code.size(), k.file, k.offset)) {
                gfx_missing++;
                fmt::print("{}: not in the game data\n", name);
                continue;
            }
            cs.push_back(k);
            gfx_added++;
        }
        fs::remove_all(tmp, ec);
    }
    std::sort(cs.begin(), cs.end(), [](auto& a, auto& b) { return a.hash < b.hash; });
    std::vector<std::string> variants;
    {
        std::ifstream in(dump / "variants.txt");
        std::set<std::string> seen;
        for (std::string line; std::getline(in, line);) {
            unsigned long long base;
            u32 v[6];
            if (std::sscanf(line.c_str(), "%llx %x %x %x %x %x %x", &base, &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) ==
                7) {
                const std::string s = fmt::format("    {{0x{:016x}ull, {{{:#x}, {:#x}, {:#x}, {:#x}, {:#x}, {:#x}}}}},\n", base,
                                                  v[0], v[1], v[2], v[3], v[4], v[5]);
                if (seen.insert(s).second)
                    variants.push_back(s);
            }
        }
    }
    std::string o = "// Generated by shadertool --known; do not edit. Hashes, offsets and launch parameters only.\n"
                    "constexpr const char* kFiles[] = {\n";
    for (auto& n : names)
        o += fmt::format("    \"{}\",\n", n);
    o += "};\nconstexpr KnownProgram kPrograms[] = {\n";
    for (auto& k : cs)
        o += fmt::format("    {{0x{:016x}ull, {}, {}, {:#x}, {:#x}, {{{}, {}, {}}}, {}, {:#x}, {:#x}, 0x{:016x}ull}},\n",
                         k.hash, k.stage, k.file, k.offset, k.size, k.block[0], k.block[1], k.block[2], k.shared_bytes,
                         k.cb_offset, k.cb_size, k.cb_hash);
    if (cs.empty())
        o += "    {},\n";
    o += "};\nconstexpr KnownVariant kVariants[] = {\n";
    for (auto& v : variants)
        o += v;
    if (variants.empty())
        o += "    {},\n";
    o += "};\n";
    common::WriteFile(out_path, std::span(reinterpret_cast<const u8*>(o.data()), o.size()));
    fmt::print("vertex/pixel programs seen: {}, {} found by the harvest, {} more located, {} not in the game data\n",
               gfx_total, gfx_harvested, gfx_added, gfx_missing);
    fmt::print("compute programs seen: {}, {} located with their launch shape, {} not in the game data, {} without a "
               "known shape; {} variants\n",
               total, cs.size() - gfx_added, not_found, no_shape, variants.size());
    return 0;
}

// ---------------------------------------------------------------- direct DXBC vs FXC, on the GPU
// Runs each vertex / pixel program twice, once from FXC's bytecode (taken from a shader cache pack, where the game put
// it) and once from the direct backend's, on the same random inputs, and compares every output.
//   pixel shaders: a fixed vertex shader feeds 30 varying attributes over a 16x16 target; all render targets and depth
//   vertex shaders: 16 points of random attributes, every output captured with stream output
namespace ab {

using Microsoft::WRL::ComPtr;

struct Pack {
    std::vector<u8> data;
    std::unordered_map<u64, std::pair<size_t, u32>> dxbc;
    bool Open(const fs::path& p) {
        data = common::ReadFile(p);
        size_t pos = 8;
        while (pos + 13 <= data.size()) {
            const u8 kind = data[pos];
            u64 h;
            u32 n;
            std::memcpy(&h, &data[pos + 1], 8);
            std::memcpy(&n, &data[pos + 9], 4);
            if (pos + 13 + n > data.size())
                break;
            if (kind == 1)
                dxbc[h] = {pos + 13, n};
            pos += 13 + n;
        }
        return !dxbc.empty();
    }
    bool Get(u64 h, std::vector<u8>& out) const {
        auto it = dxbc.find(h);
        if (it == dxbc.end())
            return false;
        out.assign(&data[it->second.first], &data[it->second.first] + it->second.second);
        return true;
    }
};

struct Rng {
    u64 s = 0x9E3779B97F4A7C15ull;
    u32 Next() {
        s ^= s << 13, s ^= s >> 7, s ^= s << 17;
        return u32(s >> 16);
    }
    float F(float lo, float hi) { return lo + (hi - lo) * float(Next() & 0xFFFFFF) / float(0xFFFFFF); }
    u32 FBits(float lo, float hi) {
        const float f = F(lo, hi);
        u32 b;
        std::memcpy(&b, &f, 4);
        return b;
    }
};

class Harness {
public:
    bool Init() {
        D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &dev_,
                                     nullptr, &ctx_)))
            return false;
        // The fixed vertex shader for pixel tests: a full-screen triangle with 30 attributes varying over it.
        std::string src = "struct O { float4 pos : SV_Position;\n";
        for (int i = 0; i < 30; i++)
            src += fmt::format("float4 a{0} : ATTR{0};\n", i);
        src += "};\nO main(uint id : SV_VertexID) { O o; float2 p = float2((id << 1) & 2, id & 2);\n"
               "o.pos = float4(p * float2(2, -2) + float2(-1, 1), 0.25, 1);\n";
        for (int i = 0; i < 30; i++)
            src += fmt::format("o.a{0} = float4(p.x * {1:.3f} + {2:.3f}, p.y * {3:.3f} - {4:.3f}, (p.x - p.y) * {5:.3f}, "
                               "{6:.3f} + p.x * p.y);\n",
                               i, 0.7 + i * 0.13, i * 0.05, 1.1 - i * 0.02, i * 0.03, 0.5 + i * 0.07, 0.2 + i * 0.01);
        src += "return o; }\n";
        ComPtr<ID3DBlob> code, err;
        if (FAILED(D3DCompile(src.data(), src.size(), nullptr, nullptr, nullptr, "main", "vs_5_0", 0, 0, &code, &err)))
            return false;
        dev_->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &feed_vs_);
        D3D11_RASTERIZER_DESC rd{D3D11_FILL_SOLID, D3D11_CULL_NONE};
        rd.DepthClipEnable = TRUE;
        dev_->CreateRasterizerState(&rd, &raster_);
        D3D11_DEPTH_STENCIL_DESC dd{};
        dd.DepthEnable = TRUE;
        dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
        dev_->CreateDepthStencilState(&dd, &depth_always_);
        D3D11_SAMPLER_DESC sd{};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        dev_->CreateSamplerState(&sd, &smp_);
        sd.Filter = D3D11_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR;
        sd.ComparisonFunc = D3D11_COMPARISON_LESS;
        dev_->CreateSamplerState(&sd, &smp_cmp_);
        // Driver constants (b13): identity swizzles, 1x render scale (thirds), no alpha test, plain viewport.
        std::vector<u32> drv(4 * 128, 0);
        const float vp[4] = {1.0f, 1.0f, 0.0f, 1.0f};
        std::memcpy(drv.data(), vp, 16);
        for (int i = 0; i < 32; i++) // tex_swizzle rows follow drv_viewport (1), drv_attrib (32), drv_vbuf (16)
            drv[(1 + 32 + 16 + i) * 4] = 0xB1A, drv[(1 + 32 + 16 + i) * 4 + 1] = 3;
        drv_ = MakeCb(drv);
        return true;
    }

    double ms_a = 0, ms_b = 0, worst_b = 0; // time to run (create + first draw + readback): FXC's, direct's
    // 0: same, 1: outputs differ, 2: could not run. `why` says where.
    int Run(bool vs, const std::vector<u8>& a, const std::vector<u8>& b, std::string& why) {
        std::vector<float> ra, rb;
        // timed: create, first draw and readback, i.e. the driver's whole compile (cold when the blobs are salted)
        const auto t0 = std::chrono::steady_clock::now();
        const bool ok_a = Execute(vs, a, a, ra, why);
        const auto t1 = std::chrono::steady_clock::now();
        const bool ok_b = ok_a && Execute(vs, b, a, rb, why);
        const auto t2 = std::chrono::steady_clock::now();
        ms_a += std::chrono::duration<double, std::milli>(t1 - t0).count();
        ms_b += std::chrono::duration<double, std::milli>(t2 - t1).count();
        worst_b = std::max(worst_b, std::chrono::duration<double, std::milli>(t2 - t1).count());
        if (!ok_a || !ok_b)
            return 2;
        if (ra.size() != rb.size()) {
            why = "output size";
            return 1;
        }
        size_t bad = 0, first = 0;
        for (size_t i = 0; i < ra.size(); i++) {
            const float x = ra[i], y = rb[i];
            if (std::memcmp(&x, &y, 4) == 0 || (std::isnan(x) && std::isnan(y)))
                continue;
            if (std::fabs(x - y) <= 1e-3f * std::max({1.0f, std::fabs(x), std::fabs(y)}))
                continue;
            if (!bad++)
                first = i;
        }
        if (bad) {
            why = fmt::format("{} of {} values differ, first at {} (fxc {} direct {})", bad, ra.size(), first, ra[first],
                              rb[first]);
            return 1;
        }
        return 0;
    }

private:
    ComPtr<ID3D11Device> dev_;
    ComPtr<ID3D11DeviceContext> ctx_;
    ComPtr<ID3D11VertexShader> feed_vs_;
    ComPtr<ID3D11RasterizerState> raster_;
    ComPtr<ID3D11DepthStencilState> depth_always_;
    ComPtr<ID3D11SamplerState> smp_, smp_cmp_;
    ComPtr<ID3D11Buffer> drv_;
    std::map<std::pair<int, int>, ComPtr<ID3D11ShaderResourceView>> textures_; // (dimension, return type)
    ComPtr<ID3D11ShaderResourceView> raw_;
    std::map<u32, ComPtr<ID3D11Buffer>> cbs_; // by size

    ComPtr<ID3D11Buffer> MakeCb(const std::vector<u32>& data) {
        D3D11_BUFFER_DESC bd{u32(data.size() * 4), D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER};
        D3D11_SUBRESOURCE_DATA init{data.data()};
        ComPtr<ID3D11Buffer> b;
        dev_->CreateBuffer(&bd, &init, &b);
        return b;
    }
    ID3D11Buffer* Cb(u32 bytes) {
        bytes = std::max<u32>((bytes + 15) & ~15u, 16);
        auto& b = cbs_[bytes];
        if (!b) {
            Rng r;
            r.s ^= bytes;
            std::vector<u32> d(bytes / 4);
            for (u32& x : d)
                x = (r.Next() & 3) == 0 ? r.Next() % 64 : r.FBits(-2.0f, 2.0f); // mostly floats, some small ints
            b = MakeCb(d);
        }
        return b.Get();
    }
    ID3D11ShaderResourceView* Texture(D3D_SRV_DIMENSION dim, D3D_RESOURCE_RETURN_TYPE rt) {
        auto& v = textures_[{int(dim), int(rt)}];
        if (v)
            return v.Get();
        const DXGI_FORMAT fmt = rt == D3D_RETURN_TYPE_UINT   ? DXGI_FORMAT_R32G32B32A32_UINT
                                : rt == D3D_RETURN_TYPE_SINT ? DXGI_FORMAT_R32G32B32A32_SINT
                                                             : DXGI_FORMAT_R32G32B32A32_FLOAT;
        Rng r;
        r.s ^= u64(dim) * 977 + u64(rt);
        const bool is_float = fmt == DXGI_FORMAT_R32G32B32A32_FLOAT;
        std::vector<u32> texels(8 * 8 * 8 * 4 * 6);
        for (u32& t : texels)
            t = is_float ? r.FBits(0.0f, 1.0f) : r.Next() % 1000;
        D3D11_SUBRESOURCE_DATA init[6 * 4];
        auto fill = [&](u32 n, u32 pitch, u32 slice) {
            for (u32 i = 0; i < n; i++)
                init[i] = {texels.data(), pitch, slice};
        };
        D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.Format = fmt;
        sv.ViewDimension = dim;
        if (dim == D3D_SRV_DIMENSION_BUFFER) {
            D3D11_BUFFER_DESC bd{u32(texels.size() * 4), D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE};
            ComPtr<ID3D11Buffer> buf;
            D3D11_SUBRESOURCE_DATA bi{texels.data()};
            dev_->CreateBuffer(&bd, &bi, &buf);
            sv.Buffer.NumElements = u32(texels.size() / 4);
            dev_->CreateShaderResourceView(buf.Get(), &sv, &v);
            return v.Get();
        }
        if (dim == D3D_SRV_DIMENSION_TEXTURE3D) {
            D3D11_TEXTURE3D_DESC td{8, 8, 8, 1, fmt, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE};
            fill(1, 8 * 16, 8 * 8 * 16);
            ComPtr<ID3D11Texture3D> t;
            dev_->CreateTexture3D(&td, init, &t);
            sv.Texture3D.MipLevels = 1;
            dev_->CreateShaderResourceView(t.Get(), &sv, &v);
            return v.Get();
        }
        const bool d1 = dim == D3D_SRV_DIMENSION_TEXTURE1D || dim == D3D_SRV_DIMENSION_TEXTURE1DARRAY;
        const bool cube = dim == D3D_SRV_DIMENSION_TEXTURECUBE || dim == D3D_SRV_DIMENSION_TEXTURECUBEARRAY;
        const bool array = dim == D3D_SRV_DIMENSION_TEXTURE1DARRAY || dim == D3D_SRV_DIMENSION_TEXTURE2DARRAY ||
                           dim == D3D_SRV_DIMENSION_TEXTURECUBEARRAY;
        const u32 layers = cube ? (array ? 12 : 6) : array ? 2 : 1;
        if (d1) {
            D3D11_TEXTURE1D_DESC td{8, 1, layers, fmt, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE};
            fill(layers, 8 * 16, 0);
            ComPtr<ID3D11Texture1D> t;
            dev_->CreateTexture1D(&td, init, &t);
            if (array)
                sv.Texture1DArray = {0, 1, 0, layers};
            else
                sv.Texture1D = {0, 1};
            dev_->CreateShaderResourceView(t.Get(), &sv, &v);
            return v.Get();
        }
        D3D11_TEXTURE2D_DESC td{8, 8, 1, layers, fmt, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0,
                                cube ? D3D11_RESOURCE_MISC_TEXTURECUBE : 0u};
        fill(layers, 8 * 16, 0);
        ComPtr<ID3D11Texture2D> t;
        dev_->CreateTexture2D(&td, init, &t);
        if (cube && array)
            sv.TextureCubeArray = {0, 1, 0, layers / 6};
        else if (cube)
            sv.TextureCube = {0, 1};
        else if (array)
            sv.Texture2DArray = {0, 1, 0, layers};
        else
            sv.Texture2D = {0, 1};
        dev_->CreateShaderResourceView(t.Get(), &sv, &v);
        return v.Get();
    }
    ID3D11ShaderResourceView* Raw() {
        if (!raw_) {
            Rng r;
            std::vector<u32> d(16384);
            for (u32& x : d)
                x = (r.Next() & 3) == 0 ? r.Next() % 64 : r.FBits(-2.0f, 2.0f);
            D3D11_BUFFER_DESC bd{u32(d.size() * 4), D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0,
                                 D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS};
            D3D11_SUBRESOURCE_DATA init{d.data()};
            ComPtr<ID3D11Buffer> b;
            dev_->CreateBuffer(&bd, &init, &b);
            D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
            sv.Format = DXGI_FORMAT_R32_TYPELESS;
            sv.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
            sv.BufferEx = {0, u32(d.size()), D3D11_BUFFEREX_SRV_FLAG_RAW};
            dev_->CreateShaderResourceView(b.Get(), &sv, &raw_);
        }
        return raw_.Get();
    }
    // Binds what the program's reflection lists (from FXC's bytecode: both builds use the same registers).
    bool Bind(bool vs, const std::vector<u8>& code, std::string& why) {
        ComPtr<ID3D11ShaderReflection> refl;
        if (FAILED(D3DReflect(code.data(), code.size(), __uuidof(ID3D11ShaderReflection), &refl))) {
            why = "no reflection";
            return false;
        }
        D3D11_SHADER_DESC sd;
        refl->GetDesc(&sd);
        ID3D11Buffer* cbs[14] = {};
        ID3D11ShaderResourceView* srvs[48] = {};
        ID3D11SamplerState* smps[16] = {};
        for (u32 i = 0; i < sd.BoundResources; i++) {
            D3D11_SHADER_INPUT_BIND_DESC b;
            refl->GetResourceBindingDesc(i, &b);
            if (b.Type == D3D_SIT_CBUFFER && b.BindPoint < 14) {
                if (b.BindPoint == 13) {
                    cbs[13] = drv_.Get();
                } else {
                    D3D11_SHADER_BUFFER_DESC cd;
                    refl->GetConstantBufferByName(b.Name)->GetDesc(&cd);
                    cbs[b.BindPoint] = Cb(cd.Size);
                }
            } else if (b.Type == D3D_SIT_TEXTURE && b.BindPoint < 48) {
                srvs[b.BindPoint] = Texture(b.Dimension, b.ReturnType);
            } else if (b.Type == D3D_SIT_BYTEADDRESS && b.BindPoint < 48) {
                srvs[b.BindPoint] = Raw();
            } else if (b.Type == D3D_SIT_SAMPLER && b.BindPoint < 16) {
                smps[b.BindPoint] = (b.uFlags & D3D_SIF_COMPARISON_SAMPLER) ? smp_cmp_.Get() : smp_.Get();
            } else {
                why = fmt::format("resource type {}", int(b.Type));
                return false;
            }
        }
        if (vs) {
            ctx_->VSSetConstantBuffers(0, 14, cbs);
            ctx_->VSSetShaderResources(0, 48, srvs);
            ctx_->VSSetSamplers(0, 16, smps);
        } else {
            ctx_->PSSetConstantBuffers(0, 14, cbs);
            ctx_->PSSetShaderResources(0, 48, srvs);
            ctx_->PSSetSamplers(0, 16, smps);
        }
        return true;
    }
    template <class T>
    void ReadBack(ID3D11Resource* res, const T& desc_src, std::vector<float>& out, u32 row_bytes, u32 rows) {
        (void)desc_src;
        ComPtr<ID3D11Resource> st;
        ComPtr<ID3D11Texture2D> t2;
        ComPtr<ID3D11Buffer> b;
        if (SUCCEEDED(res->QueryInterface(IID_PPV_ARGS(&t2)))) {
            D3D11_TEXTURE2D_DESC td;
            t2->GetDesc(&td);
            td.Usage = D3D11_USAGE_STAGING, td.BindFlags = 0, td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            ComPtr<ID3D11Texture2D> s;
            dev_->CreateTexture2D(&td, nullptr, &s);
            st = s;
        } else if (SUCCEEDED(res->QueryInterface(IID_PPV_ARGS(&b)))) {
            D3D11_BUFFER_DESC bd;
            b->GetDesc(&bd);
            bd.Usage = D3D11_USAGE_STAGING, bd.BindFlags = 0, bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ, bd.MiscFlags = 0;
            ComPtr<ID3D11Buffer> s;
            dev_->CreateBuffer(&bd, nullptr, &s);
            st = s;
        }
        ctx_->CopyResource(st.Get(), res);
        D3D11_MAPPED_SUBRESOURCE m;
        if (FAILED(ctx_->Map(st.Get(), 0, D3D11_MAP_READ, 0, &m)))
            return;
        for (u32 y = 0; y < rows; y++) {
            const float* p = reinterpret_cast<const float*>(static_cast<const u8*>(m.pData) + y * m.RowPitch);
            out.insert(out.end(), p, p + row_bytes / 4);
        }
        ctx_->Unmap(st.Get(), 0);
    }
    // `binds`: FXC's bytecode, whose reflection (RDEF) lists the resources; the direct backend writes none.
    bool Execute(bool vs, const std::vector<u8>& code, const std::vector<u8>& binds, std::vector<float>& out,
                 std::string& why) {
        ctx_->ClearState();
        if (!Bind(vs, binds, why))
            return false;
        ComPtr<ID3D11ShaderReflection> refl;
        D3DReflect(code.data(), code.size(), __uuidof(ID3D11ShaderReflection), &refl);
        D3D11_SHADER_DESC sd;
        refl->GetDesc(&sd);
        if (vs) {
            ComPtr<ID3D11VertexShader> shader;
            if (FAILED(dev_->CreateVertexShader(code.data(), code.size(), nullptr, &shader))) {
                why = "CreateVertexShader";
                return false;
            }
            // vertex attributes: one float4/uint4/int4 per ATTR element, 16 random vertices
            std::vector<D3D11_INPUT_ELEMENT_DESC> layout;
            std::vector<std::string> names;
            names.reserve(64);
            u32 stride = 0;
            std::vector<D3D_REGISTER_COMPONENT_TYPE> types;
            for (u32 i = 0; i < sd.InputParameters; i++) {
                D3D11_SIGNATURE_PARAMETER_DESC p;
                refl->GetInputParameterDesc(i, &p);
                if (p.SystemValueType != D3D_NAME_UNDEFINED)
                    continue;
                names.push_back(p.SemanticName);
                const DXGI_FORMAT f = p.ComponentType == D3D_REGISTER_COMPONENT_UINT32   ? DXGI_FORMAT_R32G32B32A32_UINT
                                      : p.ComponentType == D3D_REGISTER_COMPONENT_SINT32 ? DXGI_FORMAT_R32G32B32A32_SINT
                                                                                         : DXGI_FORMAT_R32G32B32A32_FLOAT;
                layout.push_back({names.back().c_str(), p.SemanticIndex, f, 0, stride, D3D11_INPUT_PER_VERTEX_DATA, 0});
                types.push_back(p.ComponentType);
                stride += 16;
            }
            ComPtr<ID3D11InputLayout> il;
            if (!layout.empty()) {
                if (FAILED(dev_->CreateInputLayout(layout.data(), u32(layout.size()), code.data(), code.size(), &il))) {
                    why = "CreateInputLayout";
                    return false;
                }
                Rng r;
                std::vector<u32> verts(16 * stride / 4);
                for (u32 v = 0; v < 16; v++)
                    for (u32 e = 0; e < types.size(); e++)
                        for (u32 c = 0; c < 4; c++)
                            verts[(v * stride + e * 16) / 4 + c] =
                                types[e] == D3D_REGISTER_COMPONENT_FLOAT32 ? r.FBits(-2.0f, 2.0f) : r.Next() % 8;
                D3D11_BUFFER_DESC bd{u32(verts.size() * 4), D3D11_USAGE_DEFAULT, D3D11_BIND_VERTEX_BUFFER};
                D3D11_SUBRESOURCE_DATA init{verts.data()};
                ComPtr<ID3D11Buffer> vb;
                dev_->CreateBuffer(&bd, &init, &vb);
                ID3D11Buffer* vbs[1] = {vb.Get()};
                const UINT strides[1] = {stride}, offsets[1] = {0};
                ctx_->IASetVertexBuffers(0, 1, vbs, strides, offsets);
                ctx_->IASetInputLayout(il.Get());
            }
            // every output, four components each, through stream output
            std::vector<D3D11_SO_DECLARATION_ENTRY> so;
            std::vector<std::string> onames;
            onames.reserve(64);
            for (u32 i = 0; i < sd.OutputParameters; i++) {
                D3D11_SIGNATURE_PARAMETER_DESC p;
                refl->GetOutputParameterDesc(i, &p);
                onames.push_back(p.SemanticName);
                so.push_back({0, onames.back().c_str(), p.SemanticIndex, 0, 4, 0});
            }
            const UINT so_stride = u32(so.size()) * 16;
            ComPtr<ID3D11GeometryShader> gs;
            if (FAILED(dev_->CreateGeometryShaderWithStreamOutput(code.data(), code.size(), so.data(), u32(so.size()),
                                                                  &so_stride, 1, D3D11_SO_NO_RASTERIZED_STREAM, nullptr,
                                                                  &gs))) {
                why = "stream output";
                return false;
            }
            D3D11_BUFFER_DESC bd{so_stride * 16, D3D11_USAGE_DEFAULT, D3D11_BIND_STREAM_OUTPUT};
            ComPtr<ID3D11Buffer> sob;
            dev_->CreateBuffer(&bd, nullptr, &sob);
            ID3D11Buffer* sobs[1] = {sob.Get()};
            const UINT so_off[1] = {0};
            ctx_->SOSetTargets(1, sobs, so_off);
            ctx_->VSSetShader(shader.Get(), nullptr, 0);
            ctx_->GSSetShader(gs.Get(), nullptr, 0);
            ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
            ctx_->Draw(16, 0);
            ctx_->SOSetTargets(0, nullptr, nullptr);
            ReadBack(sob.Get(), 0, out, so_stride * 16, 1);
            return true;
        }
        ComPtr<ID3D11PixelShader> shader;
        if (FAILED(dev_->CreatePixelShader(code.data(), code.size(), nullptr, &shader))) {
            why = "CreatePixelShader";
            return false;
        }
        std::vector<ComPtr<ID3D11Texture2D>> rts;
        std::vector<ComPtr<ID3D11RenderTargetView>> rtvs(8);
        ComPtr<ID3D11Texture2D> depth;
        ComPtr<ID3D11DepthStencilView> dsv;
        for (u32 i = 0; i < sd.OutputParameters; i++) {
            D3D11_SIGNATURE_PARAMETER_DESC p;
            refl->GetOutputParameterDesc(i, &p);
            if (p.SystemValueType == D3D_NAME_DEPTH || std::string(p.SemanticName) == "SV_Depth") {
                D3D11_TEXTURE2D_DESC td{16, 16, 1, 1, DXGI_FORMAT_D32_FLOAT, {1, 0}, D3D11_USAGE_DEFAULT,
                                        D3D11_BIND_DEPTH_STENCIL};
                dev_->CreateTexture2D(&td, nullptr, &depth);
                dev_->CreateDepthStencilView(depth.Get(), nullptr, &dsv);
                ctx_->ClearDepthStencilView(dsv.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);
                continue;
            }
            if (p.Register >= 8)
                continue;
            D3D11_TEXTURE2D_DESC td{16, 16, 1, 1, DXGI_FORMAT_R32G32B32A32_FLOAT, {1, 0}, D3D11_USAGE_DEFAULT,
                                    D3D11_BIND_RENDER_TARGET};
            ComPtr<ID3D11Texture2D> t;
            dev_->CreateTexture2D(&td, nullptr, &t);
            dev_->CreateRenderTargetView(t.Get(), nullptr, &rtvs[p.Register]);
            const float clear[4] = {-7, -7, -7, -7};
            ctx_->ClearRenderTargetView(rtvs[p.Register].Get(), clear);
            rts.push_back(t);
        }
        ID3D11RenderTargetView* views[8];
        for (int i = 0; i < 8; i++)
            views[i] = rtvs[i].Get();
        ctx_->OMSetRenderTargets(8, views, dsv.Get());
        ctx_->OMSetDepthStencilState(depth_always_.Get(), 0);
        D3D11_VIEWPORT vp{0, 0, 16, 16, 0, 1};
        ctx_->RSSetViewports(1, &vp);
        ctx_->RSSetState(raster_.Get());
        ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx_->VSSetShader(feed_vs_.Get(), nullptr, 0);
        ctx_->PSSetShader(shader.Get(), nullptr, 0);
        ctx_->Draw(3, 0);
        for (auto& t : rts)
            ReadBack(t.Get(), 0, out, 16 * 16, 16);
        if (depth)
            ReadBack(depth.Get(), 0, out, 16 * 4, 16);
        return true;
    }
};

} // namespace ab

// A copy of `blob` the driver has not seen: dcl_temps raised by `salt` (no effect on what it computes), checksum redone.
// Driver shader caches key on the bytecode, so this measures a cold compile.
static std::vector<u8> Salted(std::vector<u8> blob, u32 salt) {
    u32 nchunks;
    std::memcpy(&nchunks, &blob[28], 4);
    for (u32 c = 0; c < nchunks; c++) {
        u32 off;
        std::memcpy(&off, &blob[32 + c * 4], 4);
        if (std::memcmp(&blob[off], "SHEX", 4) && std::memcmp(&blob[off], "SHDR", 4))
            continue;
        u32 size;
        std::memcpy(&size, &blob[off + 4], 4);
        for (u32 pos = off + 8 + 8; pos + 8 <= off + 8 + size;) { // past the version and length tokens
            u32 tok;
            std::memcpy(&tok, &blob[pos], 4);
            const u32 op = tok & 0x7FF, len = (tok >> 24) & 0x7F;
            if (op == 104) { // dcl_temps
                u32 n;
                std::memcpy(&n, &blob[pos + 4], 4);
                n += salt;
                std::memcpy(&blob[pos + 4], &n, 4);
                break;
            }
            if (!len)
                break;
            pos += len * 4;
        }
    }
    u32 sum[4];
    gpu::dxbc::Checksum(blob.data(), blob.size(), sum);
    std::memcpy(&blob[4], sum, 16);
    return blob;
}

static int DxbcSurvey(const fs::path& dump, const fs::path& out_dir, int limit, const fs::path& pack_path) {
    const u32 salt = std::getenv("SALT") ? u32(std::atoi(std::getenv("SALT")))
                                         : 1 + u32(std::chrono::steady_clock::now().time_since_epoch().count() % 40);
    ab::Pack pack;
    if (!pack_path.empty())
        pack.Open(pack_path);
    double create_direct = 0, worst_direct = 0, create_fxc = 0, create_direct_paired = 0;
    int paired = 0;
    ID3D11Device* dev = nullptr;
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &dev, nullptr, nullptr);
    if (!out_dir.empty())
        fs::create_directories(out_dir);
    std::map<std::string, std::pair<int, std::string>> fails; // message -> count, example
    int total = 0, ok = 0, untranslated = 0, rejected = 0, loops = 0, loops_ok = 0;
    double ms = 0, worst = 0;
    for (auto& e : fs::directory_iterator(dump)) {
        const std::string name = e.path().filename().string();
        if (name.size() != 23 || !(name.ends_with(".vs.bin") || name.ends_with(".ps.bin")))
            continue;
        if (total >= limit)
            break;
        const bool vs = name.ends_with(".vs.bin");
        const Stage st = vs ? Stage::Vertex : Stage::Fragment;
        const auto data = common::ReadFile(e.path());
        TranslateOptions o;
        auto r = Translate(st, data.data(), data.size(), o);
        if (r.ok && r.uses_loop) { // as the shader cache: loop shaders sample with explicit LOD 0
            o.force_lod0 = true;
            r = Translate(st, data.data(), data.size(), o);
        }
        if (!r.ok) {
            untranslated++;
            continue;
        }
        total++;
        loops += r.uses_loop;
        std::vector<u8> blob;
        std::string err;
        const auto t0 = std::chrono::steady_clock::now();
        const bool good = gpu::dxbc::CompileHlsl(r.hlsl, vs ? gpu::dxbc::Stage::Vertex : gpu::dxbc::Stage::Pixel, blob, err);
        const double t = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        ms += t, worst = std::max(worst, t);
        if (!good) {
            auto& f = fails[err.substr(0, 90)];
            if (f.first++ == 0) {
                f.second = name;
                if (!out_dir.empty())
                    common::WriteFile(out_dir / (name + ".hlsl"), std::span((const u8*)r.hlsl.data(), r.hlsl.size()));
            }
            continue;
        }
        HRESULT hr = S_OK;
        auto create = [&](const std::vector<u8>& code) {
            const auto c0 = std::chrono::steady_clock::now();
            HRESULT h = S_OK;
            if (vs) {
                ID3D11VertexShader* s = nullptr;
                h = dev->CreateVertexShader(code.data(), code.size(), nullptr, &s);
                if (s)
                    s->Release();
            } else {
                ID3D11PixelShader* s = nullptr;
                h = dev->CreatePixelShader(code.data(), code.size(), nullptr, &s);
                if (s)
                    s->Release();
            }
            return std::make_pair(h, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - c0).count());
        };
        if (dev) {
            auto [h, t] = create(Salted(blob, salt));
            hr = h;
            create_direct += t;
            worst_direct = std::max(worst_direct, t);
            std::vector<u8> fxc;
            if (pack.dxbc.size() && pack.Get(std::strtoull(name.substr(0, 16).c_str(), nullptr, 16), fxc)) {
                auto [h2, t2] = create(Salted(fxc, salt));
                create_fxc += t2, create_direct_paired += t;
                paired++;
            }
        }
        if (FAILED(hr)) {
            auto& f = fails[fmt::format("driver rejected ({:#x})", u32(hr))];
            if (f.first++ == 0) {
                f.second = name;
                if (!out_dir.empty()) {
                    common::WriteFile(out_dir / (name + ".hlsl"), std::span((const u8*)r.hlsl.data(), r.hlsl.size()));
                    common::WriteFile(out_dir / (name + ".dxbc"), blob);
                }
            }
            rejected++;
            continue;
        }
        ok++;
        loops_ok += r.uses_loop;
    }
    std::vector<std::pair<int, std::string>> sorted;
    for (auto& [msg, f] : fails)
        sorted.push_back({f.first, fmt::format("{:5} {}  (e.g. {})", f.first, msg, f.second)});
    std::sort(sorted.rbegin(), sorted.rend());
    for (auto& [n, line] : sorted)
        fmt::print("{}\n", line);
    fmt::print("{} of {} programs compiled and accepted ({} of {} loop shaders), {} rejected by the driver, {} not "
               "translatable; {:.1f} ms in all, worst {:.1f} ms\n",
               ok, total, loops_ok, loops, rejected, untranslated, ms, worst);
    fmt::print("driver object creation (cold: salted bytecode): direct {:.0f} ms in all, worst {:.0f} ms; {} also built "
               "by FXC: FXC's {:.0f} ms vs direct's {:.0f} ms\n",
               create_direct, worst_direct, paired, create_fxc, create_direct_paired);
    if (dev)
        dev->Release();
    return 0;
}


// --dxbc-ab <shader_dump dir> <pack_vNN.bin> [limit]: FXC (from the pack) vs direct DXBC on the GPU, per program.
static int DxbcAb(const fs::path& dump, const fs::path& pack_path, int limit, u32 salt) {
    ab::Pack pack;
    if (!pack.Open(pack_path)) {
        fmt::print("no bytecode in {}\n", pack_path.string());
        return 1;
    }
    ab::Harness h;
    if (!h.Init()) {
        fmt::print("no device\n");
        return 1;
    }
    int same = 0, differ = 0, unrun = 0, nofxc = 0, nodirect = 0, n = 0;
    std::map<std::string, int> unrun_why;
    for (auto& e : fs::directory_iterator(dump)) {
        const std::string name = e.path().filename().string();
        if (name.size() != 23 || !(name.ends_with(".vs.bin") || name.ends_with(".ps.bin")))
            continue;
        if (n >= limit)
            break;
        const bool vs = name.ends_with(".vs.bin");
        const Stage st = vs ? Stage::Vertex : Stage::Fragment;
        const u64 hash = std::strtoull(name.substr(0, 16).c_str(), nullptr, 16);
        std::vector<u8> fxc;
        if (!pack.Get(hash, fxc)) {
            nofxc++;
            continue;
        }
        const auto data = common::ReadFile(e.path());
        TranslateOptions o;
        auto r = Translate(st, data.data(), data.size(), o);
        if (r.ok && r.uses_loop) {
            o.force_lod0 = true;
            r = Translate(st, data.data(), data.size(), o);
        }
        std::vector<u8> direct;
        std::string err;
        if (!r.ok || !gpu::dxbc::CompileHlsl(r.hlsl, vs ? gpu::dxbc::Stage::Vertex : gpu::dxbc::Stage::Pixel, direct, err)) {
            nodirect++;
            continue;
        }
        n++;
        std::string why;
        if (salt) {
            fxc = Salted(fxc, salt);
            direct = Salted(direct, salt);
        }
        switch (h.Run(vs, fxc, direct, why)) {
        case 0: same++; break;
        case 1:
            differ++;
            fmt::print("DIFF {} {}{}\n", name, why, r.uses_loop ? " (loop)" : "");
            break;
        default:
            unrun++;
            unrun_why[why]++;
            break;
        }
    }
    for (auto& [w, c] : unrun_why)
        fmt::print("not run: {} x{}\n", w, c);
    fmt::print("run time (create + first draw + readback): FXC {:.0f} ms, direct {:.0f} ms (worst {:.0f} ms)\n",
               h.ms_a, h.ms_b, h.worst_b);
    fmt::print("{} programs: {} same, {} differ, {} not run; skipped {} without FXC bytecode, {} the direct backend "
               "cannot build\n",
               n, same, differ, unrun, nofxc, nodirect);
    return 0;
}

// --brx-check <shader_dump dir> <game dir>: every vertex / pixel program with an indirect branch (BRX), translated
// with a constant buffer 1 holding its jump tables (found in the game's shader packages next to the program: runs
// of offsets that all land on instructions of that program) and compiled by FXC, as the shader cache would at draw.
static int BrxCheck(const fs::path& dump, const fs::path& game) {
    std::vector<std::pair<std::string, std::vector<u8>>> files;
    for (auto& e : fs::recursive_directory_iterator(game / "romfs" / "SHADER"))
        if (e.is_regular_file())
            files.push_back({e.path().filename().string(), common::ReadFile(e.path())});
    int with_brx = 0, located = 0, tables = 0, translated = 0, compiled = 0;
    std::map<std::string, int> why;
    for (auto& e : fs::directory_iterator(dump)) {
        const std::string name = e.path().filename().string();
        if (name.size() != 23 || !(name.ends_with(".vs.bin") || name.ends_with(".ps.bin")))
            continue;
        const auto data = common::ReadFile(e.path());
        const size_t size = ProgramSize(data.data(), data.size());
        if (!UsesBrx(data.data(), size))
            continue;
        with_brx++;
        const auto sites = FindBrxSites(data.data(), size);
        if (sites.empty()) {
            why["BRX without a recognised table load"]++;
            continue;
        }
        // the package holding this program
        const std::vector<u8>* file = nullptr;
        size_t at = 0;
        for (auto& [fname, blob] : files) {
            auto it = std::search(blob.begin(), blob.end(), data.begin(), data.begin() + std::min<size_t>(size, 128));
            if (it != blob.end()) {
                file = &blob, at = size_t(it - blob.begin());
                break;
            }
        }
        if (!file) {
            why["program not in the shader packages"]++;
            continue;
        }
        located++;
        u32 c1_size = 0;
        for (auto& s : sites)
            c1_size = std::max(c1_size, s.cb_off + s.entries * 4);
        std::vector<u8> c1(c1_size, 0);
        bool all = true;
        for (auto& s : sites) {
            // nearest run (by distance from the program) whose every entry lands on an instruction of the program
            long long best = -1, best_dist = 0;
            for (size_t p = 0; p + s.entries * 4 <= file->size(); p += 4) {
                bool ok = true;
                for (u32 k = 0; k < s.entries && ok; k++) {
                    u32 v;
                    std::memcpy(&v, &(*file)[p + k * 4], 4);
                    u32 t = s.base + v;
                    if (t % 32 == 0)
                        t += 8;
                    ok = t % 8 == 0 && t >= 0x58 && t < size;
                }
                if (!ok)
                    continue;
                const long long dist = std::llabs((long long)p - (long long)at);
                if (best < 0 || dist < best_dist)
                    best = (long long)p, best_dist = dist;
            }
            if (best < 0) {
                all = false;
                break;
            }
            std::memcpy(&c1[s.cb_off], &(*file)[size_t(best)], s.entries * 4);
        }
        if (!all) {
            why["jump table not found in the package"]++;
            continue;
        }
        tables++;
        const bool vs = name.ends_with(".vs.bin");
        const Stage st = vs ? Stage::Vertex : Stage::Fragment;
        TranslateOptions o;
        o.cbuf1 = c1.data();
        o.cbuf1_size = u32(c1.size());
        auto r = Translate(st, data.data(), data.size(), o);
        if (r.ok && r.uses_loop) { // as the shader cache does
            o.force_lod0 = true;
            r = Translate(st, data.data(), data.size(), o);
        }
        if (!r.ok) {
            why["translate: " + r.error.substr(0, 60)]++;
            continue;
        }
        translated++;
        std::string err;
        const UINT saved = g_flags;
        g_flags = D3DCOMPILE_OPTIMIZATION_LEVEL1 | D3DCOMPILE_IEEE_STRICTNESS; // the game's flags
        const bool ok = CompileHlsl(r.hlsl, vs ? "vs_5_0" : "ps_5_0", err);
        g_flags = saved;
        if (ok)
            compiled++;
        else
            why["FXC: " + err.substr(err.find("error") == std::string::npos ? 0 : err.find("error"), 60)]++;
        fmt::print("{} {} BRX, table(s) found, translated{}, FXC {}\n", name, sites.size(), r.uses_loop ? " (loop)" : "",
                   ok ? "ok" : "FAILED");
    }
    for (auto& [w, n] : why)
        fmt::print("  {} x{}\n", w, n);
    fmt::print("{} programs with BRX: {} located in the packages, {} with their tables, {} translated, {} compiled by FXC\n",
               with_brx, located, tables, translated, compiled);
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        fmt::print("usage: shadertool <file.bin|dir> [--print|--disasm]\n");
        return 1;
    }
    // --harvest <romfs dir> <shader_dump dir>: add every program the game ships to the prewarm manifest
    if (std::string(argv[1]) == "--harvest" && argc >= 4) {
        fmt::print("{} new programs\n", HarvestPrograms(fs::u8path(argv[2]), fs::u8path(argv[3])));
        return 0;
    }
    // --known <shader_dump dir> <game dir> <out.inc>: what a played shader_dump knows that the game data does not
    // (compute program locations and launch shapes, integer variants), as known_programs.inc
    if (std::string(argv[1]) == "--known" && argc >= 5)
        return WriteKnown(fs::u8path(argv[2]), fs::u8path(argv[3]), fs::u8path(argv[4]));
    // --dxbc-survey <shader_dump dir> [out dir] [limit]: every vertex/pixel program through the direct DXBC compiler,
    // translated as the shader cache does it; failures counted by message (one example HLSL each in out dir), the
    // rest checked by the driver.
    // --warm <shader_dump dir> <pack> <salt> <limit> [wait ms]: create (no draw) the driver objects of the first `limit`
    // vertex / pixel programs from the pack, salted, then exit: does the driver's cache keep them without a draw?
    if (std::string(argv[1]) == "--warm" && argc >= 6) {
        ab::Pack pack;
        pack.Open(fs::u8path(argv[3]));
        const u32 salt = u32(std::stoul(argv[4]));
        const int limit = std::stoi(argv[5]);
        Microsoft::WRL::ComPtr<ID3D11Device> dev;
        D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
        D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &dev, nullptr, nullptr);
        int n = 0;
        const auto t0 = std::chrono::steady_clock::now();
        for (auto& e : fs::directory_iterator(fs::u8path(argv[2]))) {
            const std::string name = e.path().filename().string();
            if (name.size() != 23 || !(name.ends_with(".vs.bin") || name.ends_with(".ps.bin")) || n >= limit)
                continue;
            std::vector<u8> fxc;
            if (!pack.Get(std::strtoull(name.substr(0, 16).c_str(), nullptr, 16), fxc))
                continue;
            fxc = Salted(fxc, salt);
            if (name.ends_with(".vs.bin")) {
                Microsoft::WRL::ComPtr<ID3D11VertexShader> s;
                dev->CreateVertexShader(fxc.data(), fxc.size(), nullptr, &s);
            } else {
                Microsoft::WRL::ComPtr<ID3D11PixelShader> s;
                dev->CreatePixelShader(fxc.data(), fxc.size(), nullptr, &s);
            }
            n++;
        }
        fmt::print("created {} in {:.0f} ms\n", n,
                   std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
        if (argc >= 7)
            std::this_thread::sleep_for(std::chrono::milliseconds(std::stoi(argv[6])));
        return 0;
    }
    if (std::string(argv[1]) == "--brx-check" && argc >= 4)
        return BrxCheck(fs::u8path(argv[2]), fs::u8path(argv[3]));
    if (std::string(argv[1]) == "--dxbc-ab" && argc >= 4)
        return DxbcAb(fs::u8path(argv[2]), fs::u8path(argv[3]), argc >= 5 ? std::stoi(argv[4]) : 1 << 30,
                      argc >= 6 ? u32(std::stoul(argv[5])) : 0u);
    if (std::string(argv[1]) == "--dxbc-survey" && argc >= 3)
        return DxbcSurvey(fs::u8path(argv[2]), argc >= 4 ? fs::u8path(argv[3]) : fs::path(),
                          argc >= 5 ? std::stoi(argv[4]) : 1 << 30, argc >= 6 ? fs::u8path(argv[5]) : fs::path());
    // Replay a captured dispatch (MUA3_CAPTURE): --replay <capture.bin> [override.hlsl] runs the kernel built by FXC and
    // by the direct backend on the captured inputs and compares every global memory window.
    if (std::string(argv[1]) == "--replay" && argc >= 3) {
        const auto raw = common::ReadFile(fs::u8path(argv[2]));
        std::vector<u32> f(raw.size() / 4);
        std::memcpy(f.data(), raw.data(), f.size() * 4);
        size_t p = 1;
        auto get_bytes = [&]() {
            const u32 n = f[p++];
            std::vector<u8> d(n);
            std::memcpy(d.data(), &f[p], n);
            p += (n + 3) / 4;
            return d;
        };
        auto hb = get_bytes();
        std::string src(hb.begin(), hb.end());
        if (argc >= 4) {
            const auto o = common::ReadFile(fs::u8path(argv[3]));
            src.assign(o.begin(), o.end());
        }
        const u32 grid[3] = {f[p + 3], f[p + 4], f[p + 5]};
        p += 6;
        struct Cb { u32 slot; std::vector<u8> d; };
        std::vector<Cb> cbs;
        for (u32 n = f[p++]; n--;) {
            Cb c;
            c.slot = f[p++];
            c.d = get_bytes();
            cbs.push_back(std::move(c));
        }
        struct Tx { u32 slot, fmt; std::vector<u8> d; };
        std::vector<Tx> txs;
        for (u32 n = f[p++]; n--;) {
            Tx t;
            t.slot = f[p++];
            t.fmt = f[p++];
            t.d = get_bytes();
            txs.push_back(std::move(t));
        }
        struct Win { u32 slot; std::vector<u8> d; };
        std::vector<Win> wins;
        for (u32 n = f[p++]; n--;) {
            Win w;
            w.slot = f[p++];
            w.d = get_bytes();
            wins.push_back(std::move(w));
        }
        auto drv = get_bytes();

        ID3D11Device* dev = nullptr;
        ID3D11DeviceContext* ctx = nullptr;
        D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
        D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &dev, nullptr, &ctx);
        auto make_cb = [&](const std::vector<u8>& d) {
            std::vector<u8> pad((d.size() + 15) / 16 * 16 + 16, 0);
            std::memcpy(pad.data(), d.data(), d.size());
            D3D11_BUFFER_DESC bd{u32(std::min<size_t>(pad.size(), 65536)), D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
            D3D11_SUBRESOURCE_DATA init{pad.data(), 0, 0};
            ID3D11Buffer* b = nullptr;
            dev->CreateBuffer(&bd, &init, &b);
            return b;
        };
        auto make_raw_srv = [&](const std::vector<u8>& d) {
            std::vector<u8> pad(std::max<size_t>((d.size() + 15) / 16 * 16, 16), 0);
            std::memcpy(pad.data(), d.data(), d.size());
            D3D11_BUFFER_DESC bd{u32(pad.size()), D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0,
                                 D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS, 0};
            D3D11_SUBRESOURCE_DATA init{pad.data(), 0, 0};
            ID3D11Buffer* b = nullptr;
            dev->CreateBuffer(&bd, &init, &b);
            D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
            sv.Format = DXGI_FORMAT_R32_TYPELESS;
            sv.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
            sv.BufferEx = {0, u32(pad.size() / 4), D3D11_BUFFEREX_SRV_FLAG_RAW};
            ID3D11ShaderResourceView* v = nullptr;
            dev->CreateShaderResourceView(b, &sv, &v);
            return v;
        };
        ID3D11Buffer* cbufs[14]{};
        ID3D11ShaderResourceView* srvs[48]{};
        for (auto& c : cbs) {
            cbufs[c.slot] = make_cb(c.d);
            srvs[32 + c.slot] = make_raw_srv(c.d);
        }
        cbufs[13] = make_cb(drv);
        for (auto& t : txs) {
            const DXGI_FORMAT fmt = DXGI_FORMAT(t.fmt & 0xFFFF);
            const u32 bpb = t.fmt >> 16;
            std::vector<u8> pad(std::max<size_t>(t.d.size(), 16), 0);
            std::memcpy(pad.data(), t.d.data(), t.d.size());
            D3D11_BUFFER_DESC bd{u32(pad.size()), D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, 0, 0};
            D3D11_SUBRESOURCE_DATA init{pad.data(), 0, 0};
            ID3D11Buffer* b = nullptr;
            dev->CreateBuffer(&bd, &init, &b);
            D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
            sv.Format = fmt;
            sv.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
            sv.Buffer.NumElements = std::max<u32>(u32(t.d.size() / std::max<u32>(bpb, 1)), 1);
            dev->CreateShaderResourceView(b, &sv, &srvs[t.slot]);
        }
        auto run = [&](const void* code, size_t size, std::vector<std::vector<u8>>& out) {
            ID3D11ComputeShader* cs = nullptr;
            if (FAILED(dev->CreateComputeShader(code, size, nullptr, &cs)))
                return false;
            ID3D11UnorderedAccessView* uavs[8]{};
            std::vector<ID3D11Buffer*> bufs(8, nullptr);
            for (auto& w : wins) {
                std::vector<u8> pad((w.d.size() + 15) / 16 * 16, 0);
                std::memcpy(pad.data(), w.d.data(), w.d.size());
                D3D11_BUFFER_DESC bd{u32(pad.size()), D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS, 0,
                                     D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS, 0};
                D3D11_SUBRESOURCE_DATA init{pad.data(), 0, 0};
                dev->CreateBuffer(&bd, &init, &bufs[w.slot]);
                D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
                ud.Format = DXGI_FORMAT_R32_TYPELESS;
                ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
                ud.Buffer = {0, u32(pad.size() / 4), D3D11_BUFFER_UAV_FLAG_RAW};
                dev->CreateUnorderedAccessView(bufs[w.slot], &ud, &uavs[w.slot]);
            }
            ctx->CSSetShader(cs, nullptr, 0);
            ctx->CSSetConstantBuffers(0, 14, cbufs);
            ctx->CSSetShaderResources(0, 48, srvs);
            ctx->CSSetUnorderedAccessViews(0, 8, uavs, nullptr);
            ctx->Dispatch(grid[0], grid[1], grid[2]);
            out.assign(8, {});
            for (auto& w : wins) {
                D3D11_BUFFER_DESC sd{u32((w.d.size() + 15) / 16 * 16), D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0, 0};
                ID3D11Buffer* st = nullptr;
                dev->CreateBuffer(&sd, nullptr, &st);
                ctx->CopyResource(st, bufs[w.slot]);
                D3D11_MAPPED_SUBRESOURCE mp;
                ctx->Map(st, 0, D3D11_MAP_READ, 0, &mp);
                out[w.slot].assign((const u8*)mp.pData, (const u8*)mp.pData + w.d.size());
                ctx->Unmap(st, 0);
                st->Release();
                uavs[w.slot]->Release();
                bufs[w.slot]->Release();
            }
            ID3D11UnorderedAccessView* none[8]{};
            ctx->CSSetUnorderedAccessViews(0, 8, none, nullptr);
            cs->Release();
            return true;
        };
        std::vector<u8> mine;
        std::string err;
        if (!gpu::dxbc::CompileHlsl(src, gpu::dxbc::Stage::Compute, mine, err)) {
            fmt::print("DXBC failed: {}\n", err);
            return 1;
        }
        std::vector<std::vector<u8>> a, b;
        run(mine.data(), mine.size(), b);
        ID3DBlob *fxc = nullptr, *errors = nullptr;
        const bool skip_fxc = argc >= 5 && std::string(argv[4]) == "--nofxc";
        if (!skip_fxc)
            D3DCompile(src.data(), src.size(), nullptr, nullptr, nullptr, "main", "cs_5_0", g_flags, 0, &fxc, &errors);
        if (!fxc) {
            fmt::print("no FXC reference; DXBC changed:\n");
            for (auto& w : wins) {
                u32 n = 0;
                for (size_t i = 0; i + 4 <= w.d.size(); i += 4)
                    n += std::memcmp(&w.d[i], &b[w.slot][i], 4) != 0;
                fmt::print("  window {}: {} words changed\n", w.slot, n);
            }
            return 0;
        }
        run(fxc->GetBufferPointer(), fxc->GetBufferSize(), a);
        int total = 0;
        for (auto& w : wins) {
            u32 n = 0, chg = 0;
            for (size_t i = 0; i + 4 <= w.d.size(); i += 4) {
                const bool d = std::memcmp(&a[w.slot][i], &b[w.slot][i], 4) != 0;
                chg += std::memcmp(&w.d[i], &a[w.slot][i], 4) != 0;
                if (d && n++ < 8) {
                    u32 x, y;
                    std::memcpy(&x, &a[w.slot][i], 4);
                    std::memcpy(&y, &b[w.slot][i], 4);
                    fmt::print("  window {} +{:#x}: fxc {:08x} dxbc {:08x}\n", w.slot, i, x, y);
                }
            }
            u32 fa[2] = {}, da[2] = {};
            std::memcpy(fa, a[w.slot].data(), std::min<size_t>(8, w.d.size()));
            std::memcpy(da, b[w.slot].data(), std::min<size_t>(8, w.d.size()));
            fmt::print("window {}: {} words differ ({} changed by FXC); head fxc {:08x} {:08x} dxbc {:08x} {:08x}\n", w.slot, n,
                       chg, fa[0], fa[1], da[0], da[1]);
            total += n;
        }
        return total ? 2 : 0;
    }
    // A/B test: --abtest <in.hlsl> [groups] runs a compute shader built by FXC and by the direct backend on the GPU and
    // compares what each wrote to u0 (a zeroed 64 KB raw buffer).
    if (std::string(argv[1]) == "--abtest" && argc >= 3) {
        const auto d = common::ReadFile(fs::u8path(argv[2]));
        const std::string src(d.begin(), d.end());
        const u32 groups = argc >= 4 ? u32(std::stoul(argv[3])) : 1;
        ID3DBlob *fxc = nullptr, *errors = nullptr;
        D3DCompile(src.data(), src.size(), nullptr, nullptr, nullptr, "main", "cs_5_0", g_flags, 0, &fxc, &errors);
        if (!fxc) {
            fmt::print("FXC failed: {}\n", errors ? std::string((const char*)errors->GetBufferPointer()) : "");
            return 1;
        }
        std::vector<u8> mine;
        std::string err;
        if (!gpu::dxbc::CompileHlsl(src, gpu::dxbc::Stage::Compute, mine, err)) {
            fmt::print("DXBC failed: {}\n", err);
            return 1;
        }
        ID3D11Device* dev = nullptr;
        ID3D11DeviceContext* ctx = nullptr;
        D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
        D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &dev, nullptr, &ctx);
        auto run = [&](const void* code, size_t size, std::vector<u32>& out) {
            ID3D11ComputeShader* cs = nullptr;
            if (FAILED(dev->CreateComputeShader(code, size, nullptr, &cs)))
                return false;
            const u32 bytes = 65536;
            std::vector<u32> zero(bytes / 4, 0);
            D3D11_BUFFER_DESC bd{bytes, D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS, 0,
                                 D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS, 0};
            D3D11_SUBRESOURCE_DATA init{zero.data(), 0, 0};
            ID3D11Buffer* buf = nullptr;
            dev->CreateBuffer(&bd, &init, &buf);
            D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
            ud.Format = DXGI_FORMAT_R32_TYPELESS;
            ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
            ud.Buffer = {0, bytes / 4, D3D11_BUFFER_UAV_FLAG_RAW};
            ID3D11UnorderedAccessView* uav = nullptr;
            dev->CreateUnorderedAccessView(buf, &ud, &uav);
            D3D11_BUFFER_DESC sd{bytes, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0, 0};
            ID3D11Buffer* st = nullptr;
            dev->CreateBuffer(&sd, nullptr, &st);
            ctx->CSSetShader(cs, nullptr, 0);
            ctx->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
            ctx->Dispatch(groups, 1, 1);
            ctx->CopyResource(st, buf);
            D3D11_MAPPED_SUBRESOURCE mp;
            ctx->Map(st, 0, D3D11_MAP_READ, 0, &mp);
            out.assign((const u32*)mp.pData, (const u32*)mp.pData + bytes / 4);
            ctx->Unmap(st, 0);
            st->Release();
            uav->Release();
            buf->Release();
            cs->Release();
            return true;
        };
        std::vector<u32> a, b;
        if (!run(fxc->GetBufferPointer(), fxc->GetBufferSize(), a) || !run(mine.data(), mine.size(), b)) {
            fmt::print("CreateComputeShader failed\n");
            return 1;
        }
        int diffs = 0;
        for (size_t i = 0; i < a.size(); i++)
            if (a[i] != b[i] && diffs++ < 24)
                fmt::print("  word {}: fxc {:08x} dxbc {:08x}\n", i, a[i], b[i]);
        fmt::print("{} differing words\n", diffs);
        return diffs ? 2 : 0;
    }
    // Direct DXBC: --dxbc <in.hlsl | compute_*.bin> <out.dxbc> compiles with the FXC-free backend, disassembles the
    // result and checks that the D3D11 runtime accepts it.
    if (std::string(argv[1]) == "--dxbc" && argc >= 4) {
        const fs::path in = fs::u8path(argv[2]);
        std::string src;
        gpu::dxbc::Stage dstage = gpu::dxbc::Stage::Compute;
        if (in.extension() == ".bin") {
            const auto data = common::ReadFile(in);
            TranslateOptions o;
            o.block[0] = 64;
            o.shared_bytes = 0x400;
            unsigned off = 0, bx = 0, by = 0, bz = 0, sh = 0;
            if (std::sscanf(in.filename().string().c_str(), "compute_%x_%ux%ux%u_%x", &off, &bx, &by, &bz, &sh) == 5)
                o.block[0] = bx, o.block[1] = by, o.block[2] = bz, o.shared_bytes = sh;
            fs::path cbp = in;
            cbp.replace_extension(".cb1");
            std::vector<u8> cb1;
            if (fs::exists(cbp)) {
                cb1 = common::ReadFile(cbp);
                o.cbuf1 = cb1.data();
                o.cbuf1_size = u32(cb1.size());
            }
            o.force_lod0 = true;
            const std::string fname = in.filename().string();
            if (fname.ends_with(".vs.bin") || fname.ends_with(".ps.bin")) { // vertex / pixel: as the shader cache does
                o = {};
                dstage = fname.ends_with(".vs.bin") ? gpu::dxbc::Stage::Vertex : gpu::dxbc::Stage::Pixel;
                const Stage st = dstage == gpu::dxbc::Stage::Vertex ? Stage::Vertex : Stage::Fragment;
                auto r0 = Translate(st, data.data(), data.size(), o);
                o.force_lod0 = r0.ok && r0.uses_loop;
            }
            auto r = Translate(dstage == gpu::dxbc::Stage::Vertex   ? Stage::Vertex
                               : dstage == gpu::dxbc::Stage::Pixel ? Stage::Fragment
                                                                   : Stage::Compute,
                               data.data(), data.size(), o);
            if (!r.ok) {
                fmt::print("translate failed: {}\n", r.error);
                return 1;
            }
            src = r.hlsl;
        } else {
            const auto d = common::ReadFile(in);
            src.assign(d.begin(), d.end());
        }
        std::vector<u8> blob;
        std::string err;
        const auto t0 = std::chrono::steady_clock::now();
        const bool ok = gpu::dxbc::CompileHlsl(src, dstage, blob, err);
        const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        if (!ok) {
            fmt::print("DXBC compile failed: {}\n", err);
            return 1;
        }
        common::WriteFile(fs::u8path(argv[3]), blob);
        fmt::print("compiled in {:.1f} ms, {} bytes\n", ms, blob.size());
        if (argc < 5 || std::string(argv[4]) != "--quiet") {
            ID3DBlob* text = nullptr;
            if (SUCCEEDED(D3DDisassemble(blob.data(), blob.size(), 0, nullptr, &text)) && text) {
                fmt::print("{}\n", std::string((const char*)text->GetBufferPointer(), text->GetBufferSize()));
                text->Release();
            } else {
                fmt::print("D3DDisassemble failed\n");
            }
        }
        ID3D11Device* dev = nullptr;
        D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
        if (SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &dev,
                                        nullptr, nullptr))) {
            ID3D11ComputeShader* cs = nullptr;
            const HRESULT hr = dev->CreateComputeShader(blob.data(), blob.size(), nullptr, &cs);
            fmt::print("CreateComputeShader: {:#x}\n", u32(hr));
            if (cs)
                cs->Release();
            dev->Release();
        }
        return 0;
    }
    // DXBC oracle: --oracle <in.hlsl> <target> <out.dxbc> compiles with FXC, saves the blob and prints its disassembly
    // and SHEX tokens; --disdxbc <in.dxbc> disassembles any blob (e.g. one the DXBC backend produced).
    if ((std::string(argv[1]) == "--oracle" && argc >= 5) || (std::string(argv[1]) == "--disdxbc" && argc >= 3)) {
        std::vector<u8> blob;
        if (std::string(argv[1]) == "--oracle") {
            const auto d = common::ReadFile(fs::u8path(argv[2]));
            ID3DBlob *code = nullptr, *errors = nullptr;
            D3DCompile(d.data(), d.size(), nullptr, nullptr, nullptr, "main", argv[3], g_flags, 0, &code, &errors);
            if (errors) {
                fmt::print("{}\n", std::string((const char*)errors->GetBufferPointer(), errors->GetBufferSize()));
                errors->Release();
            }
            if (!code)
                return 1;
            blob.assign((const u8*)code->GetBufferPointer(), (const u8*)code->GetBufferPointer() + code->GetBufferSize());
            code->Release();
            common::WriteFile(fs::u8path(argv[4]), blob);
        } else {
            blob = common::ReadFile(fs::u8path(argv[2]));
        }
        ID3DBlob* text = nullptr;
        if (SUCCEEDED(D3DDisassemble(blob.data(), blob.size(), 0, nullptr, &text)) && text) {
            fmt::print("{}\n", std::string((const char*)text->GetBufferPointer(), text->GetBufferSize()));
            text->Release();
        } else {
            fmt::print("D3DDisassemble failed\n");
        }
        u32 nchunks = 0;
        std::memcpy(&nchunks, &blob[28], 4);
        for (u32 c = 0; c < nchunks; c++) {
            u32 off, size;
            std::memcpy(&off, &blob[32 + c * 4], 4);
            std::memcpy(&size, &blob[off + 4], 4);
            fmt::print("chunk {} '{}' size {}\n", c, std::string((const char*)&blob[off], 4), size);
            if (std::memcmp(&blob[off], "SHEX", 4) == 0 || std::memcmp(&blob[off], "SHDR", 4) == 0) {
                for (u32 i = 0; i < size / 4; i++) {
                    u32 t;
                    std::memcpy(&t, &blob[off + 8 + i * 4], 4);
                    fmt::print("{:08x}{}", t, (i % 8 == 7) ? "\n" : " ");
                }
                fmt::print("\n");
            }
        }
        return 0;
    }
    if (std::string(argv[1]) == "--hlsl" && argc >= 4) { // compile a raw HLSL file: --hlsl <file> <target> [flags]
        if (argc >= 5)
            g_flags = UINT(std::stoul(argv[4], nullptr, 0));
        const auto d = common::ReadFile(fs::u8path(argv[2]));
        std::string err;
        const bool ok = CompileHlsl(std::string(d.begin(), d.end()), argv[3], err);
        fmt::print("{} {}\n", ok ? "OK" : "FAIL", err.substr(0, 800));
        return ok ? 0 : 1;
    }
    const fs::path in = fs::u8path(argv[1]);
    const bool print = argc > 2 && std::string(argv[2]) == "--print";
    const bool disasm = argc > 2 && std::string(argv[2]) == "--disasm";
    const bool no_compile = argc > 2 && std::string(argv[2]) == "--print-only";
    if (argc > 2 && std::string(argv[2]).rfind("--flags=", 0) == 0)
        g_flags = UINT(std::stoul(std::string(argv[2]).substr(8), nullptr, 0));
    std::vector<fs::path> files;
    if (fs::is_directory(in)) {
        for (auto& e : fs::directory_iterator(in))
            if (e.path().extension() == ".bin")
                files.push_back(e.path());
    } else {
        files.push_back(in);
    }
    std::map<std::string, int> unimpl;
    int ok = 0, fail = 0;
    for (auto& f : files) {
        auto data = common::ReadFile(f);
        const bool vs = f.string().find(".vs.") != std::string::npos;
        if (disasm) {
            Disassemble(data);
            continue;
        }
        const bool cs = f.filename().string().rfind("compute_", 0) == 0;
        const Stage stage = cs ? Stage::Compute : vs ? Stage::Vertex : Stage::Fragment;
        TranslateOptions copt;
        if (cs) { // compute_<offset>_<X>x<Y>x<Z>_<shared hex>.bin carries the launch shape
            copt.block[0] = 64;
            copt.shared_bytes = 0x400;
            unsigned off = 0, bx = 0, by = 0, bz = 0, sh = 0;
            if (std::sscanf(f.filename().string().c_str(), "compute_%x_%ux%ux%u_%x", &off, &bx, &by, &bz, &sh) == 5) {
                copt.block[0] = bx, copt.block[1] = by, copt.block[2] = bz;
                copt.shared_bytes = sh;
            }
            fs::path cbp = f;
            cbp.replace_extension(".cb1");
            if (fs::exists(cbp)) {
                static std::vector<u8> cb1;
                cb1 = common::ReadFile(cbp);
                copt.cbuf1 = cb1.data();
                copt.cbuf1_size = u32(cb1.size());
            }
        }
        auto r = Translate(stage, data.data(), data.size(), copt);
        if (r.uses_loop) { // like the game: loop shaders sample with explicit LOD 0
            TranslateOptions opt = copt;
            opt.force_lod0 = true;
            r = Translate(stage, data.data(), data.size(), opt);
        }
        for (auto& u : r.unimplemented)
            unimpl[u]++;
        std::string err;
        if (no_compile) {
            fmt::print("{}\n", r.hlsl);
            continue;
        }
        const bool compiled = r.ok && CompileHlsl(r.hlsl, cs ? "cs_5_0" : vs ? "vs_5_0" : "ps_5_0", err);
        if (print)
            fmt::print("{}\n", r.hlsl);
        if (compiled)
            ok++;
        else {
            fail++;
            fmt::print("FAIL {}: {}\n", f.filename().string(), (r.ok ? err : r.error).substr(0, 400));
        }
    }
    fmt::print("\n{} ok, {} failed\n", ok, fail);
    for (auto& [k, v] : unimpl)
        fmt::print("  {:24} {}\n", k, v);
}
