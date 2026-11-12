#include "gpu/d3d11/frame_capture.h"

#include "gpu/d3d11/renderer_d3d11.h"
#include "gpu/d3d11/texture_cache.h"
#include "gpu/gpu.h"

#include <fstream>

namespace gpu {

using Microsoft::WRL::ComPtr;

namespace {

u64 Fnv(const void* p, size_t n) {
    u64 h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < n; i++)
        h = (h ^ static_cast<const u8*>(p)[i]) * 0x100000001b3ull;
    return h ^ n;
}

// Bytes per texel, or per 4x4 block for block-compressed formats (bc = true); 0: not written.
u32 FormatBytes(DXGI_FORMAT f, bool& bc) {
    bc = false;
    switch (f) {
    case DXGI_FORMAT_BC1_TYPELESS: case DXGI_FORMAT_BC1_UNORM: case DXGI_FORMAT_BC1_UNORM_SRGB:
    case DXGI_FORMAT_BC4_TYPELESS: case DXGI_FORMAT_BC4_UNORM: case DXGI_FORMAT_BC4_SNORM:
        bc = true; return 8;
    case DXGI_FORMAT_BC2_TYPELESS: case DXGI_FORMAT_BC2_UNORM: case DXGI_FORMAT_BC2_UNORM_SRGB:
    case DXGI_FORMAT_BC3_TYPELESS: case DXGI_FORMAT_BC3_UNORM: case DXGI_FORMAT_BC3_UNORM_SRGB:
    case DXGI_FORMAT_BC5_TYPELESS: case DXGI_FORMAT_BC5_UNORM: case DXGI_FORMAT_BC5_SNORM:
    case DXGI_FORMAT_BC6H_TYPELESS: case DXGI_FORMAT_BC6H_UF16: case DXGI_FORMAT_BC6H_SF16:
    case DXGI_FORMAT_BC7_TYPELESS: case DXGI_FORMAT_BC7_UNORM: case DXGI_FORMAT_BC7_UNORM_SRGB:
        bc = true; return 16;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS: case DXGI_FORMAT_R32G32B32A32_FLOAT: case DXGI_FORMAT_R32G32B32A32_UINT:
    case DXGI_FORMAT_R32G32B32A32_SINT:
        return 16;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R16G16B16A16_UNORM:
    case DXGI_FORMAT_R16G16B16A16_UINT: case DXGI_FORMAT_R16G16B16A16_SNORM: case DXGI_FORMAT_R16G16B16A16_SINT:
    case DXGI_FORMAT_R32G32_TYPELESS: case DXGI_FORMAT_R32G32_FLOAT: case DXGI_FORMAT_R32G32_UINT: case DXGI_FORMAT_R32G32_SINT:
    case DXGI_FORMAT_R32G8X24_TYPELESS: case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
        return 8;
    case DXGI_FORMAT_R8_TYPELESS: case DXGI_FORMAT_R8_UNORM: case DXGI_FORMAT_R8_UINT: case DXGI_FORMAT_R8_SNORM:
    case DXGI_FORMAT_R8_SINT: case DXGI_FORMAT_A8_UNORM:
        return 1;
    case DXGI_FORMAT_R8G8_TYPELESS: case DXGI_FORMAT_R8G8_UNORM: case DXGI_FORMAT_R8G8_UINT: case DXGI_FORMAT_R8G8_SNORM:
    case DXGI_FORMAT_R8G8_SINT: case DXGI_FORMAT_R16_TYPELESS: case DXGI_FORMAT_R16_FLOAT: case DXGI_FORMAT_D16_UNORM:
    case DXGI_FORMAT_R16_UNORM: case DXGI_FORMAT_R16_UINT: case DXGI_FORMAT_R16_SNORM: case DXGI_FORMAT_R16_SINT:
    case DXGI_FORMAT_B5G6R5_UNORM: case DXGI_FORMAT_B5G5R5A1_UNORM: case DXGI_FORMAT_B4G4R4A4_UNORM:
        return 2;
    case DXGI_FORMAT_UNKNOWN: case DXGI_FORMAT_R32G32B32_TYPELESS: case DXGI_FORMAT_R32G32B32_FLOAT:
    case DXGI_FORMAT_R32G32B32_UINT: case DXGI_FORMAT_R32G32B32_SINT:
        return 0;
    default:
        return f <= DXGI_FORMAT_B8G8R8X8_UNORM_SRGB ? 4 : 0; // the remaining common formats are 32-bit
    }
}

// A 2D (array, cube) or 3D staging texture as a DDS file with a DX10 header.
void WriteDds(const fs::path& path, const TexDesc& td, ID3D11DeviceContext* ctx, ID3D11Resource* st) {
    bool bc = false;
    const u32 bpe = FormatBytes(td.format, bc);
    std::vector<u8> out;
    auto put = [&](u32 v) { out.insert(out.end(), reinterpret_cast<u8*>(&v), reinterpret_cast<u8*>(&v) + 4); };
    const bool vol = td.depth > 1 || td.volume;
    put(0x20534444); // "DDS "
    put(124);
    put(0x1 | 0x2 | 0x4 | 0x1000 | 0x20000 | (vol ? 0x800000 : 0)); // caps, height, width, pixel format, mips, depth
    put(td.height), put(td.width), put(0), put(vol ? td.depth : 1), put(td.mips);
    for (int i = 0; i < 11; i++)
        put(0);
    put(32), put(0x4), put(0x30315844), put(0), put(0), put(0), put(0), put(0); // pixel format: FourCC "DX10"
    put(0x1000 | 0x400000 | (td.mips > 1 ? 0x8 : 0)), put(vol ? 0x200000u : td.cube ? 0xFE00u : 0u), put(0), put(0), put(0);
    put(u32(td.format)), put(vol ? 4u : 3u), put(td.cube ? 4u : 0u), put(td.cube ? td.array / 6 : td.array), put(0);
    for (u32 a = 0; a < td.array; a++)
        for (u32 mip = 0; mip < td.mips; mip++) {
            const u32 w = std::max(td.width >> mip, 1u), h = std::max(td.height >> mip, 1u);
            const u32 d = vol ? std::max(td.depth >> mip, 1u) : 1u;
            const u32 row = bc ? (w + 3) / 4 * bpe : w * bpe, rows = bc ? (h + 3) / 4 : h;
            D3D11_MAPPED_SUBRESOURCE mp;
            const UINT sub = D3D11CalcSubresource(mip, a, td.mips);
            if (FAILED(ctx->Map(st, sub, D3D11_MAP_READ, 0, &mp))) {
                out.resize(out.size() + size_t(row) * rows * d);
                continue;
            }
            for (u32 z = 0; z < d; z++)
                for (u32 y = 0; y < rows; y++) {
                    const u8* p = static_cast<const u8*>(mp.pData) + size_t(z) * mp.DepthPitch + size_t(y) * mp.RowPitch;
                    out.insert(out.end(), p, p + row);
                }
            ctx->Unmap(st, sub);
        }
    common::WriteFile(path, out);
}

std::string Hex(u64 v) { return fmt::format("\"{:#x}\"", v); }

} // namespace

FrameCapture::FrameCapture(fs::path dir, u64 max_bytes)
    : dir_(std::move(dir)), budget_{max_bytes * 2 / 5, max_bytes * 2 / 5, max_bytes / 5} {
    std::error_code ec;
    for (const char* sub : {"frame", "frame/buf", "frame/tex", "frame/rt", "frame/shaders"})
        fs::create_directories(dir_ / sub, ec);
}

FrameCapture::~FrameCapture() = default;

std::string FrameCapture::SaveBuffer(u64 gpu_va, u64 size, const char* kind) {
    if (!gpu_va || !size)
        return {};
    if (size > (64u << 20)) {
        skipped_bytes_ += size;
        return {};
    }
    // big vertex streams: one read per range (static meshes are drawn many times a frame)
    const u64 key = gpu_va * 31 + size;
    if (kind[0] == 'v')
        if (auto it = ranges_.find(key); it != ranges_.end())
            return it->second;
    std::vector<u8> data(size);
    if (G().ReadGpuPartial(gpu_va, data.data(), data.size()) != size)
        return {};
    const std::string rel = SaveData(data, kind);
    if (kind[0] == 'v' && !rel.empty())
        ranges_[key] = rel;
    return rel;
}

// Contents already saved (the same constants bound by many draws) cost nothing more; only new contents count
// against the budget.
std::string FrameCapture::SaveData(const std::vector<u8>& data, const char* kind) {
    if (data.empty())
        return {};
    const u64 h = Fnv(data.data(), data.size());
    if (auto it = saved_.find(h); it != saved_.end())
        return it->second;
    if (used_[kBuf] + data.size() > budget_[kBuf]) {
        skipped_bytes_ += data.size();
        return {};
    }
    const std::string rel = fmt::format("buf/{}_{:016x}.bin", kind, h);
    common::WriteFile(dir_ / "frame" / rel, data);
    used_[kBuf] += data.size();
    return saved_[h] = rel;
}

void FrameCapture::FlushDraw() {
    if (!cur_open_)
        return;
    events_ += cur_ + (cur_dispatch_ ? ",\"gmem\":[" : ",\"tex\":[") + cur_tex_ + "]}\n";
    cur_open_ = cur_dispatch_ = false;
}

void FrameCapture::BeginDraw(const DrawState& m) {
    FlushDraw();
    const u64 state_index = states_.size() / sizeof(DrawState);
    states_.insert(states_.end(), reinterpret_cast<const u8*>(&m), reinterpret_cast<const u8*>(&m) + sizeof(m));
    const DrawCall& d = m.draw;
    std::string rts;
    for (u32 i = 0; i < 8; i++)
        if (m.color[i].format && m.color[i].addr) {
            rts += fmt::format("{}[{},{},{},{},{}]", rts.empty() ? "" : ",", i, Hex(m.color[i].addr), m.color[i].format,
                               m.color[i].width, m.color[i].height);
            if (m.color_mask[i] && std::find(targets_.begin(), targets_.end(), m.color[i].addr) == targets_.end())
                targets_.push_back(m.color[i].addr);
        }
    std::string cbs;
    for (u32 st : {kCbVertex, kCbFragment})
        for (u32 i = 0; i < 18; i++) {
            const ConstBufferBinding& b = m.cb[st][i];
            if (!b.enabled || !b.address)
                continue;
            const std::string f = SaveBuffer(b.address, std::min<u32>(b.size, 0x10000), "cb");
            cbs += fmt::format("{}[\"{}\",{},{},{},\"{}\"]", cbs.empty() ? "" : ",", st == kCbVertex ? "vs" : "ps", i,
                               Hex(b.address), b.size, f);
        }
    std::string vbs;
    for (u32 i = 0; i < 16; i++) {
        const VertexStreamState& s = m.streams[i];
        if (!s.enabled || !s.addr || s.limit < s.addr)
            continue;
        const std::string f = SaveBuffer(s.addr, s.limit - s.addr + 1, "vb");
        vbs += fmt::format("{}[{},{},{},{},\"{}\"]", vbs.empty() ? "" : ",", i, Hex(s.addr), s.limit - s.addr + 1,
                           s.stride, f);
    }
    std::string ib;
    if (d.indexed && m.index_addr && d.count && !d.indirect_args) {
        const u32 isz = m.index_format == 0 ? 1 : m.index_format == 1 ? 2 : 4;
        ib = SaveBuffer(m.index_addr + u64(d.first) * isz, u64(d.count) * isz, "ib");
    }
    cur_ = fmt::format("{{\"i\":{},\"type\":\"draw\",\"state\":{},\"topology\":{},\"indexed\":{},\"first\":{},\"count\":{},"
                       "\"indirect\":{},\"progs\":[{},{},{},{},{}],\"rt\":[{}],\"depth\":{},\"cb\":[{}],\"vb\":[{}],\"ib\":[{},{},\"{}\"]",
                       index_++, state_index, d.topology, d.indexed, d.first, d.count, Hex(d.indirect_args),
                       Hex(m.program[0]), Hex(m.program[1]), Hex(m.program[2]), Hex(m.program[3]), Hex(m.program[4]), rts,
                       m.depth.enabled ? fmt::format("[{},{},{},{}]", Hex(m.depth.addr), m.depth.format, m.depth.width,
                                                     m.depth.height)
                                       : "null",
                       cbs, vbs, Hex(m.index_addr), m.index_format, ib);
    cur_tex_.clear();
    cur_open_ = true;
    draws_++;
}

void FrameCapture::Texture(bool vs, u32 slot, u32 handle, const TicEntry& tic, ID3D11ShaderResourceView* srv,
                           bool render_target) {
    if (!cur_open_)
        return;
    std::string file;
    if (srv) {
        ComPtr<ID3D11Resource> res;
        srv->GetResource(&res);
        if (auto it = texs_.find(res.Get()); it != texs_.end()) {
            file = it->second;
        } else {
            ComPtr<ID3D11Texture2D> t2;
            ComPtr<ID3D11Texture3D> t3;
            ComPtr<ID3D11Buffer> tb;
            D3D11_TEXTURE2D_DESC d2{};
            D3D11_TEXTURE3D_DESC d3{};
            TexDesc td;
            bool ok = false, bc = false;
            if (SUCCEEDED(res.As(&t2))) {
                t2->GetDesc(&d2);
                td = {d2.Format, d2.Width, d2.Height, 1, d2.MipLevels, d2.ArraySize,
                      (d2.MiscFlags & D3D11_RESOURCE_MISC_TEXTURECUBE) != 0, false};
                ok = d2.SampleDesc.Count == 1;
            } else if (SUCCEEDED(res.As(&t3))) {
                t3->GetDesc(&d3);
                td = {d3.Format, d3.Width, d3.Height, d3.Depth, d3.MipLevels, 1, false, true};
                ok = true;
            } else if (SUCCEEDED(res.As(&tb))) { // texture buffer (TIC type 6): its guest memory, host buffer size
                D3D11_BUFFER_DESC bd{};
                tb->GetDesc(&bd);
                file = SaveBuffer(tic.Address(), bd.ByteWidth, "tb");
            }
            if (ok && FormatBytes(td.format, bc)) {
                u64 bytes = 0;
                for (u32 mip = 0; mip < td.mips; mip++) {
                    const u64 w = std::max(td.width >> mip, 1u), h = std::max(td.height >> mip, 1u),
                              d = std::max(td.depth >> mip, 1u);
                    bytes += d * (bc ? (w + 3) / 4 * ((h + 3) / 4) * FormatBytes(td.format, bc) : w * h * FormatBytes(td.format, bc));
                }
                bytes *= td.array;
                if (used_[kTex] + bytes <= budget_[kTex]) {
                    ComPtr<ID3D11Device> dev;
                    ComPtr<ID3D11DeviceContext> ctx;
                    srv->GetDevice(&dev);
                    dev->GetImmediateContext(&ctx);
                    PendingTex p;
                    p.desc = td;
                    // staging copies: CPU-readable, no bind or misc flags (the cube flag is not allowed on them)
                    HRESULT hr;
                    if (t2) {
                        d2.Usage = D3D11_USAGE_STAGING, d2.BindFlags = 0, d2.CPUAccessFlags = D3D11_CPU_ACCESS_READ, d2.MiscFlags = 0;
                        ComPtr<ID3D11Texture2D> st;
                        hr = dev->CreateTexture2D(&d2, nullptr, &st);
                        p.staging = st;
                    } else {
                        d3.Usage = D3D11_USAGE_STAGING, d3.BindFlags = 0, d3.CPUAccessFlags = D3D11_CPU_ACCESS_READ, d3.MiscFlags = 0;
                        ComPtr<ID3D11Texture3D> st;
                        hr = dev->CreateTexture3D(&d3, nullptr, &st);
                        p.staging = st;
                    }
                    if (SUCCEEDED(hr) && p.staging) {
                        ctx->CopyResource(p.staging.Get(), res.Get()); // contents as sampled by this draw
                        p.path = fmt::format("tex/{:03}_{:x}_{}x{}{}_dxgi{}{}.dds", texs_.size(), tic.Address(), td.width,
                                             td.height, td.volume ? fmt::format("x{}", td.depth) : "", u32(td.format),
                                             render_target ? "_rt" : "");
                        file = p.path;
                        used_[kTex] += bytes;
                        pending_.push_back(std::move(p));
                    }
                } else {
                    skipped_bytes_ += bytes;
                }
            }
            texs_[res.Get()] = file; // (kept alive by the cache for the frame; only compared)
        }
    }
    std::string words;
    for (u32 w : tic.w)
        words += fmt::format("{}{}", words.empty() ? "" : ",", Hex(w));
    cur_tex_ += fmt::format("{}{{\"stage\":\"{}\",\"slot\":{},\"handle\":{},\"addr\":{},\"format\":{},\"type\":{},\"w\":{},"
                            "\"h\":{},\"rt\":{},\"tic\":[{}],\"file\":\"{}\"}}",
                            cur_tex_.empty() ? "" : ",", vs ? "vs" : "ps", slot, Hex(handle), Hex(tic.Address()),
                            tic.Format(), tic.Type(), tic.Width(), tic.Height(), render_target, words, file);
}

void FrameCapture::Submit(u64 vs_hash, u64 ps_hash, u32 index_count) {
    if (!cur_open_)
        return;
    cur_ += fmt::format(",\"drawn\":true,\"vs\":\"{:016x}\",\"ps\":\"{:016x}\",\"host_count\":{}", vs_hash, ps_hash, index_count);
    shaders_.insert(vs_hash);
    if (ps_hash)
        shaders_.insert(ps_hash);
    drawn_++;
}

void FrameCapture::Dispatch(const ComputeLaunch& l) {
    FlushDraw();
    std::string cbs;
    for (u32 i = 0; i < 8; i++)
        if (l.cb_size[i])
            cbs += fmt::format("{}[{},{},{},\"{}\"]", cbs.empty() ? "" : ",", i, Hex(l.cb_addr[i]), l.cb_size[i],
                               SaveBuffer(l.cb_addr[i], std::min<u32>(l.cb_size[i], 0x10000), "cb"));
    // kept open until the next event: the renderer adds the host shader and the gmem windows
    cur_ = fmt::format("{{\"i\":{},\"type\":\"dispatch\",\"program\":{},\"offset\":{},\"grid\":[{},{},{}],\"block\":[{},{},{}],"
                       "\"shared\":{},\"cb\":[{}],\"tic_base\":{},\"tsc_base\":{}",
                       index_++, Hex(l.program), l.program_offset, l.grid[0], l.grid[1], l.grid[2], l.block[0], l.block[1],
                       l.block[2], l.shared_bytes, cbs, Hex(l.tic_base), Hex(l.tsc_base));
    cur_tex_.clear();
    cur_open_ = cur_dispatch_ = true;
    dispatches_++;
}

void FrameCapture::ComputeShader(u64 cs_hash) {
    if (!cur_open_ || !cur_dispatch_)
        return;
    cur_ += fmt::format(",\"cs\":\"{:016x}\"", cs_hash);
    shaders_.insert(cs_hash);
}

void FrameCapture::Gmem(u32 window, u64 gpu_va, const std::vector<u8>& before, const std::vector<u8>& after) {
    if (!cur_open_ || !cur_dispatch_)
        return;
    const std::string in = SaveData(before, "gmem"), out = before == after ? in : SaveData(after, "gmem");
    cur_tex_ += fmt::format("{}[{},{},{},\"{}\",\"{}\"]", cur_tex_.empty() ? "" : ",", window, Hex(gpu_va),
                            std::max(before.size(), after.size()), in, out);
}

void FrameCapture::Clear(const std::string& what) {
    FlushDraw();
    events_ += fmt::format("{{\"i\":{},\"type\":\"clear\",{}}}\n", index_++, what);
    clears_++;
}

bool FrameCapture::Present(D3D11Renderer& r, Surface* presented, u64 cpu_addr) {
    if (state_ == kArmed) {
        state_ = kRecording;
        return false;
    }
    if (state_ != kRecording)
        return false;
    FlushDraw();
    events_ += fmt::format("{{\"i\":{},\"type\":\"present\",\"cpu\":{},\"surface\":{}}}\n", index_++, Hex(cpu_addr),
                           presented ? Hex(presented->gpu_va) : "null");
    Finish(r, presented);
    state_ = kDone;
    return true;
}

void FrameCapture::Finish(D3D11Renderer& r, Surface* presented) {
    const fs::path out = dir_ / "frame";
    common::WriteFile(out / "events.jsonl", std::span(reinterpret_cast<const u8*>(events_.data()), events_.size()));
    common::WriteFile(out / "states.bin", states_);
    // textures sampled this frame
    ID3D11DeviceContext* ctx = nullptr;
    if (!pending_.empty()) {
        ComPtr<ID3D11Device> dev;
        pending_[0].staging->GetDevice(&dev);
        dev->GetImmediateContext(&ctx);
    }
    for (auto& p : pending_)
        WriteDds(out / p.path, p.desc, ctx, p.staging.Get());
    if (ctx)
        ctx->Release();
    pending_.clear();
    // final contents of every colour target written (BMP, host size), within the budget
    u32 rt_saved = 0, rt_skipped = 0;
    for (size_t i = 0; i < targets_.size(); i++) {
        Surface* s = r.FindSurfaceByGpu(targets_[i]);
        if (!s || s->depth || !s->tex)
            continue;
        D3D11_TEXTURE2D_DESC td;
        s->tex->GetDesc(&td);
        const u64 bytes = u64(td.Width) * td.Height * 3;
        if (used_[kRt] + bytes > budget_[kRt]) {
            rt_skipped++;
            skipped_bytes_ += bytes;
            continue;
        }
        used_[kRt] += bytes;
        r.DumpSurface(s, out / fmt::format("rt/{:02}_{:x}_{}x{}_fmt{:x}{}.bmp", i, s->gpu_va, s->width, s->height, s->format,
                                           s == presented ? "_present" : ""));
        rt_saved++;
    }
    if (presented)
        r.DumpSurface(presented, dir_ / "screenshot.bmp");
    // the guest shader programs used (copies from shader_dump, where they are written when first seen)
    u32 progs = 0;
    std::error_code ec;
    for (u64 h : shaders_)
        for (const char* tag : {"vs", "ps", "cs"}) {
            const fs::path src = fmt::format("shader_dump/{:016x}.{}.bin", h, tag);
            if (fs::exists(src, ec) && fs::copy_file(src, out / "shaders" / src.filename(), fs::copy_options::overwrite_existing, ec))
                progs++;
        }
    summary_ = fmt::format("{} draws ({} drawn), {} dispatches, {} clears; {} textures, {} buffers, {} target images "
                           "({} not saved), {} shader programs; {:.1f} MB written, {:.1f} MB over the budget skipped",
                           draws_, drawn_, dispatches_, clears_, texs_.size(), saved_.size(), rt_saved, rt_skipped, progs,
                           (used_[0] + used_[1] + used_[2]) / 1048576.0, skipped_bytes_ / 1048576.0);
    std::ofstream(out / "README.txt")
        << "MUA3 frame capture (see docs/bug_capture.md in the source tree)\n"
        << summary_ << "\n"
        << "events.jsonl  one JSON object per draw / dispatch / clear / present, in submission order\n"
        << "states.bin    gpu::DrawState of each draw back to back (draw 'state' = index; sizeof = " << sizeof(DrawState)
        << " bytes, src/gpu/draw_state.h of this build)\n"
        << "buf/          guest memory: constant buffers (cb_), vertex streams (vb_), index ranges (ib_), texture buffers\n"
        << "              (tb_); compute global\n"
        << "              memory windows before / after each dispatch (gmem_, host GPU contents); all by content hash\n"
        << "tex/          every texture as first sampled this frame (DDS with a DX10 header, host format)\n"
        << "rt/           colour targets written this frame, final contents (BMP)\n"
        << "shaders/      guest Maxwell programs by hash, compute (.cs.bin) too (shadertool reads them like shader_dump)\n";
}

} // namespace gpu
