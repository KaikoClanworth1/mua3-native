#include <cstdio>
// Translates dumped Maxwell shaders (shader_dump/*.vs.bin|*.ps.bin) to HLSL and compiles them with FXC.
//   shadertool <file.bin> [--print]        translate one shader and print the HLSL
//   shadertool <dir>                       translate + compile every dump, report failures/unimplemented ops
#include <d3d11.h>
#include <d3dcompiler.h>
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
            auto r = Translate(Stage::Compute, data.data(), data.size(), o);
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
        const bool ok = gpu::dxbc::CompileHlsl(src, gpu::dxbc::Stage::Compute, blob, err);
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
