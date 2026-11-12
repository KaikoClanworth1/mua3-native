#include <bit>
#include <cstring>
#include <chrono>
#include <unordered_set>
#include <fstream>
#include "gpu/d3d11/shader_cache.h"
#include "gpu/dxbc/dxbc.h"
#include "gpu/gpu.h"
#include "gpu/shader/anticipate.h"
#include "gpu/shader/known_programs.h"
#include "gpu/shader/maxwell_isa.h"
#include "loader/formats.h"

#include <cctype>
#include <cstdlib>
#include <d3dcompiler.h>
#include <windows.h>

#pragma comment(lib, "d3dcompiler.lib")

namespace gpu {

// Bump kTranslatorVersion whenever the translator output changes, to invalidate cached bytecode.
constexpr int kTranslatorVersion = 31; // 31: shadow textures past 16 get their own sampler register
// Failure markers carry this: older ones (before the direct backend took over what FXC fails on) are retried once.
constexpr u8 kFailGeneration = 1;


namespace {
const char* StageTag(shader::Stage st) {
    return st == shader::Stage::Vertex ? "vs" : st == shader::Stage::Compute ? "cs" : "ps";
}
const char* StageTarget(shader::Stage st) {
    return st == shader::Stage::Vertex ? "vs_5_0" : st == shader::Stage::Compute ? "cs_5_0" : "ps_5_0";
}
} // namespace


ShaderCache::ShaderCache(ID3D11Device* dev, unsigned build_workers) : dev_(dev) {
    LOG_INFO("Shader", "opening the shader cache");
    std::error_code ec;
    fs::create_directories(dump_dir_, ec);
    fs::create_directories(cache_dir_, ec);
    overrides_ = fs::exists("shader_override", ec);
    OpenPack();
    if (!build_workers)
        StartDriverRewarm();
    const unsigned n = build_workers ? build_workers : std::clamp(std::thread::hardware_concurrency() / 2, 2u, 6u);
    for (unsigned i = 0; i < n; i++)
        workers_.emplace_back([this] { Worker(false); });
    workers_.emplace_back([this] { Worker(false, true); });
    // Loop shaders compile out of process (FXC can run for minutes on them); two workers so one slow shader
    // does not hold up the rest, and one more for the shaders the game is waiting on. The setup's shader step
    // has the machine to itself: there they are most of its time, so one per CPU it may use.
    for (unsigned i = 0; i < (build_workers ? build_workers : 2u); i++)
        std::thread([this] { Worker(true); }).detach();
    std::thread([this] { Worker(true, true); }).detach();
}

ShaderCache::~ShaderCache() {
    {
        std::lock_guard lk(mutex_);
        stop_ = true;
    }
    cv_.notify_all();
    for (auto& t : workers_)
        t.join();
    rewarm_stop_ = true;
    if (rewarm_.joinable())
        rewarm_.join();
    if (pack_out_)
        std::fclose(pack_out_);
    if (pack_)
        UnmapViewOfFile(pack_);
    if (pack_map_)
        CloseHandle(pack_map_);
    if (pack_file_)
        CloseHandle(pack_file_);
}

void ShaderCache::Worker(bool slow, bool urgent_only) {
    SetThreadDescription(GetCurrentThread(), slow ? L"ShaderCompiler (slow)" : L"ShaderCompiler");
    auto& q = slow ? slow_queue_ : queue_;
    for (;;) {
        HostShader* s;
        {
            std::unique_lock lk(mutex_);
            // urgent shaders are at the front: an urgent-only worker sleeps while only the backlog is left
            cv_.wait(lk, [&] { return stop_ || (!q.empty() && (!urgent_only || !q.front()->prewarm)); });
            if (stop_)
                return;
            s = q.front();
            q.pop_front();
        }
        if (s->claimed.exchange(true))
            continue;
        if (Build(*s, slow) && --pending == 0)
            prewarm_end_ns = std::chrono::steady_clock::now().time_since_epoch().count();
    }
}

bool ShaderCache::Build(HostShader& s, bool slow) {
    const char* tag = StageTag(s.stage);
    if (s.persist) { // first seen this launch (Get): keep the program for the next launch's prewarm
        s.persist = false;
        const std::span<const u8> code(s.code.data(), s.code.size());
        common::WriteFile(dump_dir_ / fmt::format("{:016x}.{}.bin", s.hash, tag), code);
        PackPut(PackKind(PackProgram + StageIndex(s.stage)), s.hash, code);
        if (s.stage == shader::Stage::Compute) { // launch shape and constant buffer 1, so Prewarm rebuilds it exactly
            std::vector<u8> o(16 + s.cbuf1.size());
            const u32 shape[4] = {s.opt.block[0], s.opt.block[1], s.opt.block[2], s.opt.shared_bytes};
            std::memcpy(o.data(), shape, 16);
            if (!s.cbuf1.empty())
                std::memcpy(o.data() + 16, s.cbuf1.data(), s.cbuf1.size());
            common::WriteFile(dump_dir_ / fmt::format("{:016x}.cs.opt", s.hash), std::span(o.data(), o.size()));
            PackPut(PackOpt, s.hash, std::span(o.data(), o.size()));
        }
    }
    if (s.is_variant && !s.translated) {
        const auto t0 = std::chrono::steady_clock::now();
        BuildVariantSource(s);
        s.translated = true;
        translate_ns += u64((std::chrono::steady_clock::now() - t0).count());
    }
    // Vertex and pixel shaders with cached bytecode: the renderer's view of them comes from the cache too.
    if (!s.is_variant && !s.translated && !s.no_cache && s.stage != shader::Stage::Compute &&
        HasBytecode(s) && LoadInfoFor(s, s.info)) {
        s.translated = true;
        info_hits++;
    }
    if (!s.is_variant && !s.translated) {
        const auto t0 = std::chrono::steady_clock::now();
        s.opt.cbuf1 = s.cbuf1.empty() ? nullptr : s.cbuf1.data();
        s.info = shader::Translate(s.stage, s.code.data(), s.code.size(), s.opt);
        if (s.info.ok && (s.info.uses_loop || s.stage == shader::Stage::Compute)) {
            // Gradient ops inside loops are rejected by FXC: loop shaders sample with explicit LOD 0.
            shader::TranslateOptions opt = s.opt;
            opt.force_lod0 = true;
            s.info = shader::Translate(s.stage, s.code.data(), s.code.size(), opt);
        }
        s.translated = true;
        translate_ns += u64((std::chrono::steady_clock::now() - t0).count());
        if (s.info.ok && !s.no_cache && s.stage != shader::Stage::Compute)
            SaveInfoFor(s, s.info);
    }
    const char* target = StageTarget(s.stage);
    // Compute shaders go through the direct DXBC backend (milliseconds): no slow FXC queue for them.
    const bool direct = s.stage == shader::Stage::Compute && !std::getenv("MUA3_FXC_COMPUTE");
    // Vertex and pixel shaders: FXC, and the direct backend for what FXC fails on (Compile). MUA3_DIRECT_VSPS=1 uses
    // the direct backend first (milliseconds instead of up to minutes per loop shader), but its bytecode costs the
    // driver ~2.5x FXC's to compile at first draw, so it is not the default.
    static const bool direct_vsps = std::getenv("MUA3_DIRECT_VSPS") != nullptr;
    if (s.stage != shader::Stage::Compute && direct_vsps && !overrides_ && s.info.ok && !s.direct_tried &&
        !HasBytecode(s)) {
        s.direct_tried = true;
        std::string derr;
        const auto d0 = std::chrono::steady_clock::now();
        if (!dxbc::CompileHlsl(s.info.hlsl, s.stage == shader::Stage::Vertex ? dxbc::Stage::Vertex : dxbc::Stage::Pixel,
                               s.direct_blob, derr)) {
            s.direct_blob.clear();
            LOG_DEBUG("Shader", "{} {:016x}: direct DXBC failed ({}), using FXC", tag, s.hash, derr.substr(0, 120));
        }
        compile_ns += u64((std::chrono::steady_clock::now() - d0).count());
    }
    if (!slow && !direct && s.direct_blob.empty() && s.info.ok && s.info.uses_loop && !HasBytecode(s)) {
        s.slow = true;
        s.claimed = false;
        {
            std::lock_guard lk(mutex_);
            if (s.prewarm)
                slow_queue_.push_back(&s);
            else
                slow_queue_.push_front(&s);
        }
        cv_.notify_all();
        return false;
    }
    std::string err;
    const auto c0 = std::chrono::steady_clock::now();
    if (s.is_variant) {
        Compile(s, s.info.hlsl, err, slow);
    } else if (s.info.ok && s.info.uses_loop && std::getenv("MUA3_NO_LOOP_SHADERS")) {
        err = "loop shaders disabled (MUA3_NO_LOOP_SHADERS)";
    } else if (s.info.ok) {
        Compile(s, s.info.hlsl, err, slow);
    } else {
        err = s.info.error;
    }
    compile_ns += u64((std::chrono::steady_clock::now() - c0).count());
    s.ok = err.empty() && (s.vs || s.ps || s.cs || !s.created);
    if (!s.ok) {
        failures++;
        if (!s.is_variant && !s.no_cache) // don't retry it at every launch's prewarm
            MarkFailed(s.hash);
        bool quiet;
        {
            std::lock_guard lk(ready_mutex_);
            quiet = s.quiet;
            if (quiet)
                s.quiet_error = err.substr(0, 1500);
        }
        if (quiet) {
            LOG_DEBUG("Shader", "{} {:016x} (set up, not drawn yet) failed: {}", tag, s.hash, err.substr(0, 200));
        } else {
            common::WriteFile(dump_dir_ / fmt::format("{:016x}.{}.hlsl", s.hash, tag),
                              std::span(reinterpret_cast<const u8*>(s.info.hlsl.data()), s.info.hlsl.size()));
            LOG_ERROR("Shader", "{} {:016x} failed: {}", tag, s.hash, err.substr(0, 1500));
        }
    } else {
        std::string un;
        for (auto& u : s.info.unimplemented)
            un += u + " ";
        if (s.quiet)
            LOG_DEBUG("Shader", "{} {:016x} (set up, not drawn yet): {} bytes", tag, s.hash, s.code.size());
        else
            LOG_INFO("Shader", "{} {:016x}: {} bytes, {} textures{}{}", tag, s.hash, s.code.size(),
                     s.info.textures.size(), un.empty() ? "" : ", unimplemented: ", un);
    }
    s.code.clear();
    s.code.shrink_to_fit();
    {
        std::lock_guard lk(ready_mutex_);
        s.state = s.ok ? HostShader::Ready : HostShader::Failed;
    }
    ready_cv_.notify_all();
    return true;
}

bool ShaderCache::WaitReady(HostShader* s, std::chrono::nanoseconds budget) {
    if (s->state != HostShader::Pending)
        return true;
    if (s->slow)
        return false;
    std::unique_lock lk(ready_mutex_);
    return ready_cv_.wait_for(lk, budget, [&] { return s->state != HostShader::Pending; });
}

fs::path ShaderCache::InfoPath(const HostShader& s) const {
    return cache_dir_ / fmt::format("{:016x}_v{}.info", s.hash, kTranslatorVersion);
}

namespace {
struct InfoWriter {
    std::vector<u8> out;
    void U32(u32 v) { out.insert(out.end(), reinterpret_cast<const u8*>(&v), reinterpret_cast<const u8*>(&v) + 4); }
    void Str(const std::string& s) {
        U32(u32(s.size()));
        out.insert(out.end(), s.begin(), s.end());
    }
};
struct InfoReader {
    const std::vector<u8>& in;
    size_t pos = 0;
    bool ok = true;
    u32 U32() {
        if (pos + 4 > in.size()) {
            ok = false;
            return 0;
        }
        u32 v;
        std::memcpy(&v, &in[pos], 4);
        pos += 4;
        return v;
    }
    std::string Str() {
        const u32 n = U32();
        if (!ok || pos + n > in.size()) {
            ok = false;
            return {};
        }
        std::string s(reinterpret_cast<const char*>(&in[pos]), n);
        pos += n;
        return s;
    }
};
constexpr u32 kInfoMagic = 0x464E4953; // 'SINF'
} // namespace

std::vector<u8> ShaderCache::EncodeInfo(const shader::TranslateResult& info) {
    InfoWriter w;
    w.U32(kInfoMagic);
    w.U32(kTranslatorVersion);
    w.U32(u32(info.textures.size()));
    for (const auto& t : info.textures) {
        w.U32(t.cbuf_offset);
        w.U32(u32(t.dim) | (u32(t.array) << 8) | (u32(t.shadow) << 9) | (u32(t.is_int) << 10));
        w.U32(t.slot);
        w.U32(t.sampler);
    }
    for (u32 r : info.cbuf_rows)
        w.U32(r);
    w.U32(info.cbuf_dyn_mask);
    w.U32(info.cbuf_mask);
    w.U32(info.attrib_in_mask);
    w.U32(info.rt_mask);
    w.U32(u32(info.writes_depth) | (u32(info.uses_gmem) << 1) | (u32(info.writes_gmem) << 2) |
          (u32(info.structured) << 3) | (u32(info.uses_loop) << 4));
    w.U32(u32(info.images.size()));
    for (u32 i : info.images)
        w.U32(i);
    w.U32(u32(info.unimplemented.size()));
    for (const auto& u : info.unimplemented)
        w.Str(u);
    return std::move(w.out);
}

bool ShaderCache::DecodeInfo(const std::vector<u8>& data, shader::TranslateResult& info) {
    if (data.empty())
        return false;
    InfoReader r{data};
    if (r.U32() != kInfoMagic || r.U32() != u32(kTranslatorVersion))
        return false;
    shader::TranslateResult o;
    o.ok = true;
    const u32 ntex = r.U32();
    for (u32 i = 0; i < ntex && i < 64 && r.ok; i++) {
        shader::TextureUse t;
        t.cbuf_offset = r.U32();
        const u32 f = r.U32();
        t.dim = shader::TexDim(f & 0xFF);
        t.array = f & 0x100, t.shadow = f & 0x200, t.is_int = f & 0x400;
        t.slot = r.U32();
        t.sampler = r.U32();
        o.textures.push_back(t);
    }
    for (u32& c : o.cbuf_rows)
        c = r.U32();
    o.cbuf_dyn_mask = r.U32();
    o.cbuf_mask = r.U32();
    o.attrib_in_mask = r.U32();
    o.rt_mask = r.U32();
    const u32 b = r.U32();
    o.writes_depth = b & 1, o.uses_gmem = b & 2, o.writes_gmem = b & 4, o.structured = b & 8, o.uses_loop = b & 16;
    const u32 nimg = r.U32();
    for (u32 i = 0; i < nimg && i < 64 && r.ok; i++)
        o.images.push_back(r.U32());
    const u32 nun = r.U32();
    for (u32 i = 0; i < nun && i < 64 && r.ok; i++)
        o.unimplemented.insert(r.Str());
    if (!r.ok)
        return false;
    info = std::move(o);
    return true;
}

void ShaderCache::OpenPack() {
    const fs::path path = cache_dir_ / fmt::format("pack_v{}.bin", kTranslatorVersion);
    constexpr u32 kMagic = 0x314B5053; // 'SPK1'
    pack_file_ = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    if (pack_file_ != INVALID_HANDLE_VALUE) {
        LARGE_INTEGER size{};
        GetFileSizeEx(pack_file_, &size);
        pack_size_ = size_t(size.QuadPart);
        if (pack_size_ >= 8) {
            pack_map_ = CreateFileMappingW(pack_file_, nullptr, PAGE_READONLY, 0, 0, nullptr);
            if (pack_map_)
                pack_ = static_cast<const u8*>(MapViewOfFile(pack_map_, FILE_MAP_READ, 0, 0, 0));
        }
    } else {
        pack_file_ = nullptr;
    }
    size_t entries = 0, pos = 0;
    if (pack_) {
        // One sequential read of the whole pack: walking its entry headers through page faults read a 200 MB pack
        // from a hard disk in small pieces (24 s at launch, and the prewarm then faulted the bytecode in again).
        WIN32_MEMORY_RANGE_ENTRY range{const_cast<u8*>(pack_), pack_size_};
        PrefetchVirtualMemory(GetCurrentProcess(), 1, &range, 0);
        u32 magic = 0, version = 0;
        std::memcpy(&magic, pack_, 4);
        std::memcpy(&version, pack_ + 4, 4);
        pos = magic == kMagic && version == u32(kTranslatorVersion) ? 8 : pack_size_;
        while (pos + 13 <= pack_size_) { // kind u8, hash u64, size u32, data
            const u8 kind = pack_[pos];
            u64 hash;
            u32 size;
            std::memcpy(&hash, pack_ + pos + 1, 8);
            std::memcpy(&size, pack_ + pos + 9, 4);
            if (pos + 13 + size > pack_size_)
                break; // a partial entry at the end (crashed mid-write): ignored, overwritten by the next append
            if (kind == PackDxbc)
                pack_dxbc_[hash] = {pos + 13, size};
            else if (kind == PackInfo)
                pack_info_[hash] = {pos + 13, size};
            else if (kind == PackFail && size >= 1 && pack_[pos + 13] >= kFailGeneration)
                pack_fail_.insert(hash); // (older markers: retried once, the direct backend may build it now)
            else if (kind >= PackProgram && kind < PackProgram + 3)
                pack_prog_[kind - PackProgram][hash] = {pos + 13, size};
            else if (kind == PackOpt)
                pack_opt_[hash] = {pos + 13, size};
            pos += 13 + size;
            entries++;
        }
    }
    // Appends go after the last complete entry; a new or foreign-version pack starts over.
    const bool fresh = !pack_ || entries == 0; // (a foreign-version pack is rewritten)
    pack_out_ = _wfopen(path.c_str(), fresh ? L"wb" : L"r+b");
    if (pack_out_) {
        if (fresh) {
            const u32 hdr[2] = {kMagic, u32(kTranslatorVersion)};
            std::fwrite(hdr, 1, 8, pack_out_);
            std::fflush(pack_out_);
        } else {
            _fseeki64(pack_out_, s64(pos), SEEK_SET);
        }
    }
    LOG_INFO("Shader", "packed cache: {} entries ({} MB), {} bytecodes, {} infos, {} failures, {} programs", entries,
             pack_size_ >> 20, pack_dxbc_.size(), pack_info_.size(), pack_fail_.size(),
             pack_prog_[0].size() + pack_prog_[1].size() + pack_prog_[2].size());
}

bool ShaderCache::PackGet(PackKind kind, u64 hash, std::vector<u8>& out) const {
    const auto& map = kind == PackDxbc    ? pack_dxbc_
                      : kind == PackInfo  ? pack_info_
                      : kind == PackOpt   ? pack_opt_
                                          : pack_prog_[kind - PackProgram];
    const auto it = map.find(hash);
    if (it == map.end())
        return false;
    out.assign(pack_ + it->second.offset, pack_ + it->second.offset + it->second.size);
    return true;
}

void ShaderCache::PackPut(PackKind kind, u64 hash, std::span<const u8> data) {
    std::lock_guard lk(pack_mutex_);
    if (!pack_out_)
        return;
    const u32 size = u32(data.size());
    std::fwrite(&kind, 1, 1, pack_out_);
    std::fwrite(&hash, 1, 8, pack_out_);
    std::fwrite(&size, 1, 4, pack_out_);
    if (size)
        std::fwrite(data.data(), 1, size, pack_out_);
    std::fflush(pack_out_);
}

bool ShaderCache::HasBytecode(const HostShader& s) const {
    return pack_dxbc_.count(s.hash) || fs::exists(CachePath(s, StageTarget(s.stage)));
}

bool ShaderCache::GetBytecode(const HostShader& s, std::vector<u8>& out) {
    if (PackGet(PackDxbc, s.hash, out))
        return true;
    out = common::ReadFile(CachePath(s, StageTarget(s.stage)));
    if (out.empty())
        return false;
    PackPut(PackDxbc, s.hash, out); // folded in: the next launch reads it from the pack
    return true;
}

bool ShaderCache::LoadInfoFor(const HostShader& s, shader::TranslateResult& info) {
    std::vector<u8> data;
    if (PackGet(PackInfo, s.hash, data))
        return DecodeInfo(data, info);
    data = common::ReadFile(InfoPath(s));
    if (!DecodeInfo(data, info))
        return false;
    PackPut(PackInfo, s.hash, data);
    return true;
}

void ShaderCache::SaveInfoFor(const HostShader& s, const shader::TranslateResult& info) {
    const auto data = EncodeInfo(info);
    PackPut(PackInfo, s.hash, data);
    common::WriteFile(InfoPath(s), data);
}

bool ShaderCache::HasFailed(u64 hash) const {
    {
        std::lock_guard lk(pack_mutex_);
        if (pack_fail_.count(hash))
            return true;
    }
    return fs::exists(cache_dir_ / fmt::format("{:016x}_v{}_g{}.fail", hash, kTranslatorVersion, kFailGeneration));
}

void ShaderCache::MarkFailed(u64 hash) {
    {
        std::lock_guard lk(pack_mutex_);
        pack_fail_.insert(hash);
    }
    const u8 generation = kFailGeneration;
    PackPut(PackFail, hash, std::span(&generation, 1));
    std::ofstream(cache_dir_ / fmt::format("{:016x}_v{}_g{}.fail", hash, kTranslatorVersion, kFailGeneration));
}

// A cached base shader skipped translation: its variants are rewritten from the HLSL, so translate it now.
void ShaderCache::EnsureHlsl(HostShader& base) { // (hlsl_mutex_ held)
    if (!base.info.hlsl.empty() || base.is_variant)
        return;
    std::vector<u8> code;
    if (!PackGet(PackKind(PackProgram + StageIndex(base.stage)), base.hash, code))
        code = common::ReadFile(dump_dir_ / fmt::format("{:016x}.{}.bin", base.hash, StageTag(base.stage)));
    if (code.empty())
        return;
    shader::TranslateOptions opt = base.opt;
    auto info = shader::Translate(base.stage, code.data(), code.size(), opt);
    if (info.ok && info.uses_loop) {
        opt.force_lod0 = true;
        info = shader::Translate(base.stage, code.data(), code.size(), opt);
    }
    base.info.hlsl = std::move(info.hlsl);
}

fs::path ShaderCache::CachePath(const HostShader& s, const char* target) const {
    return cache_dir_ / fmt::format("{:016x}_{}_v{}.dxbc", s.hash, target, kTranslatorVersion);
}

bool ShaderCache::Compile(HostShader& s, const std::string& hlsl, std::string& err, bool out_of_process) {
    const char* target = StageTarget(s.stage);
    if (s.stage == shader::Stage::Compute && !std::getenv("MUA3_FXC_COMPUTE") && !s.direct_dxbc) {
        std::vector<u8> blob;
        std::string derr;
        std::string src = hlsl;
        const fs::path ovr = fs::path("shader_override") / fmt::format("{:016x}.cs.hlsl", s.hash);
        if (overrides_ && fs::exists(ovr)) {
            const auto text = common::ReadFile(ovr);
            src.assign(text.begin(), text.end());
            LOG_INFO("Shader", "using override {}", ovr.string());
        }
        if (dxbc::CompileHlsl(src, dxbc::Stage::Compute, blob, derr)) {
            const HRESULT hr = dev_->CreateComputeShader(blob.data(), blob.size(), nullptr, &s.cs);
            if (SUCCEEDED(hr)) {
                s.direct_dxbc = true;
                if (std::getenv("MUA3_DXBC_VERIFY")) {
                    // Reference build through FXC (out of process, bounded) for the renderer's A/B comparison.
                    ComPtr<ID3D11ComputeShader> mine = s.cs;
                    s.cs.Reset();
                    std::string ferr;
                    s.no_cache = true;
                    if (Compile(s, hlsl, ferr, true))
                        s.cs_ref = s.cs;
                    else
                        LOG_INFO("Shader", "cs {:016x}: no FXC reference ({})", s.hash, ferr.substr(0, 120));
                    s.cs = mine;
                }
                return true;
            }
            LOG_WARN("Shader", "cs {:016x}: driver rejected direct DXBC ({:#x}), using FXC", s.hash, u32(hr));
        } else {
            LOG_WARN("Shader", "cs {:016x}: direct DXBC failed ({}), using FXC", s.hash, derr);
        }
        s.direct_dxbc = true; // tried; the FXC path below runs once
    }
    fs::path cached = s.no_cache ? fs::path() : CachePath(s, target);
    std::vector<u8> blob;
    if (!s.no_cache && GetBytecode(s, blob))
        cache_hits++;
    // Debug aid: shader_override/<hash>.<vs|ps>.hlsl replaces the translated HLSL (never cached).
    std::string source = hlsl;
    const fs::path override_path =
        fs::path("shader_override") / fmt::format("{:016x}.{}.hlsl", s.hash, StageTag(s.stage));
    if (overrides_ && fs::exists(override_path)) {
        const auto text = common::ReadFile(override_path);
        source.assign(text.begin(), text.end());
        blob.clear();
        cached.clear();
        LOG_INFO("Shader", "using override {}", override_path.string());
    }
    if (blob.empty() && !s.direct_blob.empty()) { // built by the direct backend in Build
        blob = std::move(s.direct_blob);
        s.direct_blob.clear();
        if (!cached.empty()) {
            common::WriteFile(cached, blob);
            PackPut(PackDxbc, s.hash, blob);
        }
    }
    // FXC failed on a vertex / pixel shader (an internal compiler error on some, or a 2-minute timeout): the direct
    // backend builds it instead, so its draws are not left out.
    auto direct_fallback = [&]() {
        // (doubles: the direct backend has none, and its parse error would be a C++ exception the crash handler
        // logs with a stack trace. The only such programs are harvest hits on data no draw uses.)
        if (s.stage == shader::Stage::Compute || source.find("asdouble") != std::string::npos)
            return false;
        std::vector<u8> b;
        std::string derr;
        if (!dxbc::CompileHlsl(source, s.stage == shader::Stage::Vertex ? dxbc::Stage::Vertex : dxbc::Stage::Pixel, b,
                               derr))
            return false;
        LOG_INFO("Shader", "{} {:016x}: FXC failed ({}), built by the direct backend", StageTag(s.stage), s.hash,
                 err.substr(0, 100));
        blob = std::move(b);
        err.clear();
        if (!cached.empty()) {
            common::WriteFile(cached, blob);
            PackPut(PackDxbc, s.hash, blob);
        }
        return true;
    };
    if (blob.empty() && out_of_process) {
        // Run FXC in a child process we can kill: some loop shaders make it spin for many minutes.
        const auto tmp = fs::temp_directory_path() / fmt::format("mua3_shader_{:016x}", s.hash);
        const fs::path in = tmp.string() + ".hlsl", out = tmp.string() + ".dxbc";
        common::WriteFile(in, std::span(reinterpret_cast<const u8*>(source.data()), source.size()));
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring cmd = L"\"" + std::wstring(exe) + L"\" --compile-shader \"" + in.wstring() + L"\" \"" +
                           out.wstring() + L"\" " + std::wstring(target, target + std::strlen(target));
        STARTUPINFOW si{sizeof(si)};
        PROCESS_INFORMATION pi{};
        if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS,
                            nullptr, nullptr, &si, &pi)) {
            err = "could not start shader compiler process";
            return false;
        }
        DWORD code = 1;
        if (WaitForSingleObject(pi.hProcess, 120000) == WAIT_TIMEOUT) {
            TerminateProcess(pi.hProcess, 1);
            WaitForSingleObject(pi.hProcess, 5000);
            err = "FXC timed out (2 min)";
        } else {
            GetExitCodeProcess(pi.hProcess, &code);
        }
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        if (err.empty()) {
            if (code == 0) {
                blob = common::ReadFile(out);
                if (!cached.empty()) {
                    common::WriteFile(cached, blob);
                    PackPut(PackDxbc, s.hash, blob);
                }
            } else {
                const auto e = common::ReadFile(out.string() + ".err");
                err.assign(e.begin(), e.end());
            }
        }
        std::error_code ec;
        fs::remove(in, ec);
        fs::remove(out, ec);
        fs::remove(out.string() + ".err", ec);
        if (!err.empty() && !direct_fallback())
            return false;
    }
    if (blob.empty()) {
        ComPtr<ID3DBlob> code, errors;
        const UINT flags = D3DCOMPILE_OPTIMIZATION_LEVEL1 | D3DCOMPILE_IEEE_STRICTNESS;
        const HRESULT hr = D3DCompile(source.data(), source.size(), nullptr, nullptr, nullptr, "main", target, flags, 0,
                                      &code, &errors);
        if (FAILED(hr)) {
            err = errors ? std::string((const char*)errors->GetBufferPointer(), errors->GetBufferSize()) : "D3DCompile failed";
            if (!direct_fallback())
                return false;
        } else {
            blob.assign((const u8*)code->GetBufferPointer(), (const u8*)code->GetBufferPointer() + code->GetBufferSize());
            if (!cached.empty()) {
                common::WriteFile(cached, blob);
                PackPut(PackDxbc, s.hash, blob);
            }
        }
    }
    if (s.prewarm && s.stage != shader::Stage::Compute) {
        // Nobody is waiting for it: the driver object (~2.8 MB of driver memory each) is created at first use.
        s.dxbc = std::move(blob);
        s.created = false;
        return true;
    }
    return CreateObject(s, blob, err);
}

bool ShaderCache::CreateObject(HostShader& s, const std::vector<u8>& blob, std::string& err) {
    HRESULT hr;
    const auto k0 = std::chrono::steady_clock::now(); // the driver builds its native code here
    if (s.stage == shader::Stage::Vertex)
        hr = dev_->CreateVertexShader(blob.data(), blob.size(), nullptr, &s.vs);
    else if (s.stage == shader::Stage::Compute)
        hr = dev_->CreateComputeShader(blob.data(), blob.size(), nullptr, &s.cs);
    else
        hr = dev_->CreatePixelShader(blob.data(), blob.size(), nullptr, &s.ps);
    if (&blob != &s.dxbc && s.stage != shader::Stage::Compute)
        s.dxbc = blob; // kept: input layouts (vertex), and recreation after the object was released as unused
    create_ns += u64((std::chrono::steady_clock::now() - k0).count());
    if (FAILED(hr)) {
        err = fmt::format("Create shader failed {:#x}", u32(hr));
        return false;
    }
    return true;
}

void ShaderCache::Realize(HostShader* s) {
    if (s->created.load(std::memory_order_acquire))
        return;
    std::string err;
    const auto r0 = std::chrono::steady_clock::now();
    const bool made = CreateObject(*s, s->dxbc, err);
    realize_ns += u64((std::chrono::steady_clock::now() - r0).count());
    if (!made) {
        LOG_ERROR("Shader", "{} {:016x}: {}", StageTag(s->stage), s->hash, err);
        s->ok = false;
        s->state = HostShader::Failed;
    }
    realized++;
    live_objects++;
    s->created.store(true, std::memory_order_release);
}

// Every vertex / pixel shader's driver object created once and released: the driver compiles each in the background
// and keeps the GPU code in its disk cache (setup step; GPU thread idle). Returns how many were handed over.
u32 ShaderCache::WarmDriverCache() {
    u32 n = 0;
    for (auto& [hash, sp] : shaders_) {
        HostShader& s = *sp;
        if (s.state != HostShader::Ready || !s.ok || s.dxbc.empty() || s.stage == shader::Stage::Compute)
            continue;
        if (s.stage == shader::Stage::Vertex) {
            ComPtr<ID3D11VertexShader> o;
            dev_->CreateVertexShader(s.dxbc.data(), s.dxbc.size(), nullptr, &o);
        } else {
            ComPtr<ID3D11PixelShader> o;
            dev_->CreatePixelShader(s.dxbc.data(), s.dxbc.size(), nullptr, &o);
        }
        n++;
    }
    return n;
}

// The driver's compiler threads run in this process: done when its CPU time has stopped growing (under 3% of one
// CPU over 3 s). Returns the seconds waited.
double WaitForDriverCompiles(std::chrono::seconds cap) {
    auto cpu_s = [] {
        FILETIME c, e, k, u;
        GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
        auto v = [](const FILETIME& f) { return double((u64(f.dwHighDateTime) << 32) | f.dwLowDateTime) * 1e-7; };
        return v(k) + v(u);
    };
    const auto t0 = std::chrono::steady_clock::now();
    double last = cpu_s();
    while (std::chrono::steady_clock::now() - t0 < cap) {
        std::this_thread::sleep_for(std::chrono::seconds(3));
        const double now = cpu_s();
        if (now - last < 0.09)
            break;
        last = now;
    }
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

// "<vendor id> <driver version>" of the device's adapter ("" when unknown): a new driver starts with an empty cache.
std::string DriverVersion(ID3D11Device* dev) {
    ComPtr<IDXGIDevice> dxgi;
    ComPtr<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC desc{};
    LARGE_INTEGER umd{};
    if (FAILED(dev->QueryInterface(IID_PPV_ARGS(&dxgi))) || FAILED(dxgi->GetAdapter(&adapter)) ||
        FAILED(adapter->GetDesc(&desc)) || FAILED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd)))
        return {};
    const u64 v = u64(umd.QuadPart);
    return fmt::format("{:04x} {}.{}.{}.{}", desc.VendorId, v >> 48, (v >> 32) & 0xFFFF, (v >> 16) & 0xFFFF, v & 0xFFFF);
}

void SaveDriverVersion(ID3D11Device* dev) {
    const std::string v = DriverVersion(dev);
    if (!v.empty())
        common::WriteFile(fs::path("shader_cache") / "driver.txt", std::span((const u8*)v.data(), v.size()));
}

// Program type in a DXBC blob's SHEX/SHDR version token: 0 pixel, 1 vertex, 5 compute (~0u: not found).
static u32 DxbcProgramType(const u8* blob, u32 size) {
    if (size < 32 || std::memcmp(blob, "DXBC", 4))
        return ~0u;
    u32 n;
    std::memcpy(&n, blob + 28, 4);
    for (u32 c = 0; c < n && 32 + c * 4 + 4 <= size; c++) {
        u32 off;
        std::memcpy(&off, blob + 32 + c * 4, 4);
        if (off + 12 > size)
            break;
        if (!std::memcmp(blob + off, "SHEX", 4) || !std::memcmp(blob + off, "SHDR", 4)) {
            u32 version;
            std::memcpy(&version, blob + off + 8, 4);
            return version >> 16;
        }
    }
    return ~0u;
}

void ShaderCache::StartDriverRewarm() {
    if (!pack_ || std::getenv("MUA3_NO_DRIVER_WARM"))
        return;
    const std::string version = DriverVersion(dev_);
    if (version.empty())
        return;
    const auto saved = common::ReadFile(cache_dir_ / "driver.txt");
    if (std::string(saved.begin(), saved.end()) == version)
        return;
    // The pack's vertex / pixel bytecode, in pack order (append-only: the same order every launch).
    std::vector<std::pair<size_t, u32>> blobs;
    for (const auto& [hash, e] : pack_dxbc_) {
        const u32 type = DxbcProgramType(pack_ + e.offset, e.size);
        if (type == 0 || type == 1)
            blobs.push_back({e.offset, e.size});
    }
    std::sort(blobs.begin(), blobs.end());
    // Progress of an earlier launch with this driver ("<version>\n<index>"), a little before it to cover compiles
    // the driver had not finished when that launch stopped.
    size_t from = 0;
    const auto prog = common::ReadFile(cache_dir_ / "driver_warm.txt");
    const std::string p(prog.begin(), prog.end());
    if (const size_t nl = p.find('\n'); nl != std::string::npos && p.substr(0, nl) == version)
        from = size_t(std::max<long long>(0, std::atoll(p.c_str() + nl + 1) - 50));
    LOG_INFO("Shader", "graphics driver {} (prepared for: {}): handing {} shaders to it again from {}, until the first "
             "level loads", version, saved.empty() ? "none" : std::string(saved.begin(), saved.end()), blobs.size(), from);
    rewarm_ = std::thread([this, blobs = std::move(blobs), from, version] { DriverRewarm(blobs, from, version); });
}

void ShaderCache::DriverRewarm(std::vector<std::pair<size_t, u32>> blobs, size_t from, std::string version) {
    SetThreadDescription(GetCurrentThread(), L"ShaderDriverRewarm");
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
    // A level loading: its first frames create a hundred or more driver objects (the title about 40).
    constexpr u32 kLevelLoad = 100;
    auto save = [&](size_t i) {
        const std::string t = fmt::format("{}\n{}", version, i);
        common::WriteFile(cache_dir_ / "driver_warm.txt", std::span((const u8*)t.data(), t.size()));
    };
    size_t i = from;
    for (; i < blobs.size(); i++) {
        if (rewarm_stop_ || realized >= kLevelLoad || shader::GameLoading()) {
            save(i);
            LOG_INFO("Shader", "driver shader re-warm paused at {} of {} (a movie or level is loading); next launch "
                     "goes on", i, blobs.size());
            return;
        }
        const u8* b = pack_ + blobs[i].first;
        const u32 n = blobs[i].second;
        if (DxbcProgramType(b, n) == 1) {
            ComPtr<ID3D11VertexShader> o;
            dev_->CreateVertexShader(b, n, nullptr, &o);
        } else {
            ComPtr<ID3D11PixelShader> o;
            dev_->CreatePixelShader(b, n, nullptr, &o);
        }
        if (i % 50 == 49)
            save(i + 1);
        // ~10 a second: the driver compiles each on its own threads (~0.1 s of CPU cold), about one CPU's worth
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    // all handed over: give the driver's threads time to finish before recording this driver as prepared
    for (int s = 0; s < 60 && !rewarm_stop_ && !shader::GameLoading(); s++)
        std::this_thread::sleep_for(std::chrono::seconds(1));
    if (rewarm_stop_ || shader::GameLoading()) {
        save(blobs.size() > 50 ? blobs.size() - 50 : 0);
        return;
    }
    common::WriteFile(cache_dir_ / "driver.txt", std::span((const u8*)version.data(), version.size()));
    std::error_code ec;
    fs::remove(cache_dir_ / "driver_warm.txt", ec);
    LOG_INFO("Shader", "driver shader re-warm done: {} shaders", blobs.size());
}

// Programs the game set up since the last frame (most at boot): each is looked up, so a program that is in neither
// the pack nor the game's files (some are built at runtime) compiles now instead of at its first draw.
void ShaderCache::Anticipate() {
    constexpr size_t kPerFrame = 64; // programs looked up per frame (each a GPU memory read of its code)
    for (const u64 va : shader::TakeAnticipated())
        anticipate_pending_.push_back(va), anticipate_taken_++;
    if (frame % 600 == 0 && anticipate_taken_ != anticipate_logged_) {
        anticipate_logged_ = anticipate_taken_;
        LOG_INFO("Shader", "programs set up by the game: {}, looked up {}, {} of them new (compiled ahead), {} to go",
                 anticipate_taken_, anticipate_lookups_, anticipate_new_, anticipate_pending_.size());
    }
    static thread_local std::vector<u8> buf;
    for (size_t n = 0; n < kPerFrame && !anticipate_pending_.empty(); n++) {
        const u64 va = anticipate_pending_.front();
        anticipate_pending_.pop_front();
        // The header and first instructions first: the same program set up again (same code at the same address)
        // is skipped without reading all of it.
        u8 head[0x60];
        if (G().ReadGpuPartial(va, head, sizeof(head)) < sizeof(head))
            continue;
        u32 w0;
        std::memcpy(&w0, head, 4);
        // graphics programs only (SPH as the harvest checks it); compute programs have no header
        const u32 type = w0 & 0x1F, version = (w0 >> 5) & 0x1F, stype = (w0 >> 10) & 0xF, sass = (w0 >> 17) & 0xF;
        if (!((type == 1 && stype == 1) || (type == 2 && stype == 5)) || version != 3 || sass != 3)
            continue;
        const u64 head_hash = Hash64(head, sizeof(head));
        if (auto [it, fresh] = anticipate_seen_.try_emplace(va, head_hash); !fresh) {
            if (it->second == head_hash)
                continue;
            it->second = head_hash;
        }
        buf.assign(0x10000, 0);
        const size_t got = G().ReadGpuPartial(va, buf.data(), buf.size());
        if (got < 0x60)
            continue;
        const u32 before = new_programs;
        anticipating_ = true;
        Get(type == 1 ? shader::Stage::Vertex : shader::Stage::Fragment, buf.data(), got);
        anticipating_ = false;
        anticipate_lookups_++;
        anticipate_new_ += new_programs != before;
        new_programs = before; // (that count is programs first seen by a draw)
    }
}

HostShader* ShaderCache::Get(shader::Stage stage, const u8* code, size_t size, const shader::TranslateOptions* opt) {
    size = shader::ProgramSize(code, size);
    u64 hash = Hash64(code, size) ^ (u64(stage) << 60);
    if (opt) { // the launch shape is compiled into compute shaders
        const u32 shape[4] = {opt->block[0], opt->block[1], opt->block[2], opt->shared_bytes};
        hash ^= Hash64(shape, sizeof(shape)) * 0x9E3779B97F4A7C15ull;
    }
    // first use this launch and never listed: the next Prewarm starts with it (programs the game only set up don't
    // count: it sets up all of them at boot)
    if (!anticipating_ && ordered_.insert(hash).second)
        AppendLine(dump_dir_ / "order.txt", fmt::format("{:016x}\n", hash));
    auto it = shaders_.find(hash);
    if (it != shaders_.end()) {
        HostShader* found = it->second.get();
        if (found->quiet && !anticipating_) { // a draw asks for a program only set up so far: failures matter now
            std::lock_guard lk(ready_mutex_);
            found->quiet = false;
            if (found->state == HostShader::Failed)
                LOG_ERROR("Shader", "{} {:016x} failed: {}", StageTag(found->stage), found->hash, found->quiet_error);
        }
        if (found->prewarm && found->state == HostShader::Pending) { // still waiting in the prewarm backlog
            found->prewarm = false;
            std::lock_guard lk(mutex_);
            // already translated and handed to the slow (loop shader) queue, or still in the normal one
            (found->translated && found->info.uses_loop ? slow_queue_ : queue_).push_front(found);
            cv_.notify_all();
        }
        return found;
    }
    if (HasFailed(hash)) { // failed in an earlier run (or the setup's shader step): not compiled and logged again
        auto s = std::make_unique<HostShader>();
        s->stage = stage;
        s->hash = hash;
        s->state = HostShader::Failed;
        s->quiet = anticipating_;
        s->quiet_error = "failed in an earlier run";
        if (!anticipating_)
            LOG_WARN("Shader", "{} {:016x} failed in an earlier run: its draws are skipped", StageTag(stage), hash);
        auto* raw = s.get();
        shaders_[hash] = std::move(s);
        return raw;
    }
    new_programs++;
    HostShader* s = Queue(stage, hash, code, size, opt, false, true); // (the worker writes it to the dump and pack)
    return s;
}

HostShader* ShaderCache::Queue(shader::Stage stage, u64 hash, const u8* code, size_t size,
                               const shader::TranslateOptions* opt, bool prewarm, bool persist) {
    auto s = std::make_unique<HostShader>();
    s->prewarm = prewarm;
    s->persist = persist;
    s->quiet = anticipating_ && !prewarm;
    s->stage = stage;
    s->hash = hash;
    s->code.assign(code, code + size);
    if (opt) {
        s->opt = *opt;
        if (opt->cbuf1)
            s->cbuf1.assign(opt->cbuf1, opt->cbuf1 + opt->cbuf1_size);
        s->opt.cbuf1 = s->cbuf1.empty() ? nullptr : s->cbuf1.data();
        s->opt.cbuf1_size = u32(s->cbuf1.size());
    }
    auto* raw = s.get();
    shaders_[hash] = std::move(s);
    pending++;
    {
        std::lock_guard lk(mutex_);
        if (prewarm)
            queue_.push_back(raw);
        else
            queue_.push_front(raw); // the game is waiting for this one
    }
    cv_.notify_all(); // all: urgent-only workers skip backlog items
    return raw;
}

HostShader* ShaderCache::GetVariant(HostShader* base, const VariantKey& v) {
    if (!base || !(v.attr_uint | v.attr_sint | v.attr_snorm10 | v.tex_uint | v.tex_sint) || base->state != HostShader::Ready)
        return base;
    const u64 key = base->hash ^ (Hash64(&v, sizeof(v)) * 0x9E3779B97F4A7C15ull);
    auto it = shaders_.find(key);
    if (it != shaders_.end()) {
        HostShader* found = it->second.get();
        if (!replaying_ && found->prewarm && found->state == HostShader::Pending) { // a draw needs it now
            found->prewarm = false;
            std::lock_guard lk(mutex_);
            queue_.push_front(found);
            cv_.notify_all();
        }
        return found;
    }
    auto s = std::make_unique<HostShader>();
    s->stage = base->stage;
    s->hash = key;
    s->is_variant = true;
    s->prewarm = replaying_; // replayed from variants.txt: part of the backlog until a draw asks for it
    s->base = base;
    s->key = {v.attr_uint, v.attr_sint, v.attr_scaled, v.attr_snorm10, v.tex_uint, v.tex_sint};
    s->record = !replaying_; // newly seen (not replayed from the manifest): remember it for the next prewarm
    new_variants += !replaying_;
    auto* raw = s.get();
    shaders_[key] = std::move(s);
    pending++;
    {
        std::lock_guard lk(mutex_);
        if (replaying_)
            queue_.push_back(raw);
        else
            queue_.push_front(raw);
    }
    cv_.notify_all(); // all: urgent-only workers skip backlog items
    return raw;
}

void ShaderCache::BuildVariantSource(HostShader& s) {
    HostShader* base = s.base;
    VariantKey v;
    v.attr_uint = s.key[0], v.attr_sint = s.key[1], v.attr_scaled = s.key[2], v.attr_snorm10 = s.key[3];
    v.tex_uint = s.key[4], v.tex_sint = s.key[5];
    LOG_INFO("Shader", "variant {:016x} of {:016x} (tex uint {:#x} sint {:#x})", s.hash, base->hash, v.tex_uint, v.tex_sint);
    if (s.record)
        AppendLine(dump_dir_ / "variants.txt", fmt::format("{:016x} {:x} {:x} {:x} {:x} {:x} {:x}\n", base->hash, v.attr_uint,
                                                           v.attr_sint, v.attr_scaled, v.attr_snorm10, v.tex_uint, v.tex_sint));
    // Debug overrides of the base shader apply to its variants too.
    const fs::path override_path = fs::path("shader_override") /
        fmt::format("{:016x}.{}.hlsl", base->hash, base->stage == shader::Stage::Vertex ? "vs" : "ps");
    const bool overridden = overrides_ && fs::exists(override_path);
    {
        std::lock_guard lk(hlsl_mutex_);
        // Its bytecode is cached: Compile loads that, so only the base's resource layout is needed (re-translating
        // a base that was itself loaded from the cache costs tens of milliseconds the draw would wait for).
        if (!overridden && HasBytecode(s)) {
            s.info = base->info;
            return;
        }
        EnsureHlsl(*base);
        s.info = base->info;
    }
    std::string& src = s.info.hlsl;
    if (overridden) {
        const auto text = common::ReadFile(override_path);
        src.assign(text.begin(), text.end());
        s.no_cache = true;
    }
    // Integer vertex attributes: integer inputs (the input assembler hands the integers through unchanged).
    const size_t vsin = src.find("struct VSIn {");
    for (u32 i = 0; i < 32 && vsin != std::string::npos; i++) {
        if (!((v.attr_uint | v.attr_sint) & (1u << i)))
            continue;
        const std::string decl = fmt::format("    float4 a{0} : ATTR{0};", i);
        const size_t at = src.find(decl, vsin);
        if (at == std::string::npos)
            continue;
        src.replace(at, decl.size(), fmt::format("    {}4 a{} : ATTR{};", (v.attr_sint >> i) & 1 ? "int" : "uint", i, i));
        if ((v.attr_scaled >> i) & 1) {
            const std::string load = fmt::format("    vin{0} = asuint(input.a{0});", i);
            const size_t l = src.find(load, vsin);
            if (l != std::string::npos)
                src.replace(l, load.size(), fmt::format("    vin{0} = asuint(float4(input.a{0}));", i));
        }
    }
    // Packed signed-normalized 10:10:10:2 attributes: raw bits in, sign extension and scaling here.
    for (u32 i = 0; i < 32 && vsin != std::string::npos; i++) {
        if (!((v.attr_snorm10 >> i) & 1))
            continue;
        const std::string decl = fmt::format("    float4 a{0} : ATTR{0};", i);
        const size_t at = src.find(decl, vsin);
        if (at == std::string::npos)
            continue;
        src.replace(at, decl.size(), fmt::format("    uint4 a{0} : ATTR{0};", i));
        const std::string load = fmt::format("    vin{0} = asuint(input.a{0});", i);
        const size_t l = src.find(load, vsin);
        if (l != std::string::npos)
            src.replace(l, load.size(), fmt::format("    vin{0} = asuint(Snorm1010102(input.a{0}));", i));
    }
    if (v.attr_snorm10 && vsin != std::string::npos)
        src.insert(vsin, "float4 Snorm1010102(uint4 v) {\n"
                         "    const int4 s = int4(v << uint4(22u, 22u, 22u, 30u)) >> int4(22, 22, 22, 30);\n"
                         "    return max(float4(s) / float4(511.0f, 511.0f, 511.0f, 1.0f), -1.0f);\n}\n");
    // Integer textures: typed declarations, and loads reinterpreted so the shader sees the raw integer bits.
    for (u32 t = 0; t < 32; t++) {
        if (!((v.tex_uint | v.tex_sint) & (1u << t)))
            continue;
        const std::string elem = (v.tex_sint >> t) & 1 ? "int4" : "uint4";
        const std::string reg = fmt::format("<float4> tex{} : register(t{});", t, t);
        const size_t d = src.find(reg);
        if (d == std::string::npos)
            continue;
        src.replace(d, reg.size(), fmt::format("<{}> tex{} : register(t{});", elem, t, t));
        const std::string call = fmt::format("tex{}.Load(", t);
        for (size_t at = 0; (at = src.find(call, at)) != std::string::npos;) {
            if (at > 0 && (std::isalnum(u8(src[at - 1])) || src[at - 1] == '_')) {
                at += call.size();
                continue;
            }
            size_t q = at + call.size();
            for (int depth = 1; q < src.size() && depth; q++)
                depth += src[q] == '(' ? 1 : src[q] == ')' ? -1 : 0;
            src.insert(q, ")");
            src.insert(at, "asfloat(");
            at = q + 9;
        }
    }
}

// Small appends (order.txt, variants.txt) from the GPU thread, written by a background thread.
void ShaderCache::AppendLine(const fs::path& file, std::string line) {
    struct Appender {
        std::mutex mutex;
        std::condition_variable cv;
        std::deque<std::pair<fs::path, std::string>> lines;
        Appender() {
            std::thread([this] {
                SetThreadDescription(GetCurrentThread(), L"ShaderManifest");
                for (;;) {
                    std::pair<fs::path, std::string> one;
                    {
                        std::unique_lock lk(mutex);
                        cv.wait(lk, [&] { return !lines.empty(); });
                        one = std::move(lines.front());
                        lines.pop_front();
                    }
                    std::ofstream(one.first, std::ios::app) << one.second;
                }
            }).detach();
        }
    };
    static Appender a;
    {
        std::lock_guard lk(a.mutex);
        a.lines.emplace_back(file, std::move(line));
    }
    a.cv.notify_one();
}

bool CompileShaderFile(const char* in_path, const char* out_path, const char* target) {
    const auto src = common::ReadFile(fs::u8path(in_path));
    ComPtr<ID3DBlob> code, errors;
    const HRESULT hr = D3DCompile(src.data(), src.size(), nullptr, nullptr, nullptr, "main", target,
                                  D3DCOMPILE_OPTIMIZATION_LEVEL1 | D3DCOMPILE_IEEE_STRICTNESS, 0, &code, &errors);
    if (FAILED(hr)) {
        if (errors)
            common::WriteFile(fs::u8path(std::string(out_path) + ".err"),
                              std::span(static_cast<const u8*>(errors->GetBufferPointer()), errors->GetBufferSize()));
        return false;
    }
    common::WriteFile(fs::u8path(out_path),
                      std::span(static_cast<const u8*>(code->GetBufferPointer()), code->GetBufferSize()));
    return true;
}

void ShaderCache::Prewarm() {
    prewarm_start_ = std::chrono::steady_clock::now();
    size_t n = 0;
    std::error_code ec;
    // Programs in the order the game first asked for them (order.txt, appended by Get), then the rest: a cold
    // cache compiles the boot and menu shaders first.
    struct Prog {
        int stage; // StageIndex
        fs::path path; // empty: in the pack
    };
    std::unordered_map<u64, Prog> files;
    std::vector<u64> order;
    for (int st = 0; st < 3; st++)
        for (const auto& [h, e] : pack_prog_[st])
            files[h] = {st, {}};
    for (const auto& e : fs::directory_iterator(dump_dir_, ec)) { // dumps not yet in the pack (older launches)
        const std::string name = e.path().filename().string(); // <16 hex>.<vs|ps>.bin
        if (name.size() != 23 || !name.ends_with(".bin"))
            continue;
        const std::string tag = name.substr(17, 2);
        if (tag != "vs" && tag != "ps" && tag != "cs")
            continue;
        const u64 h = std::strtoull(name.substr(0, 16).c_str(), nullptr, 16);
        if (!files.count(h))
            files[h] = {tag == "vs" ? 0 : tag == "ps" ? 1 : 2, e.path()};
    }
    {
        std::ifstream in(dump_dir_ / "order.txt");
        std::unordered_set<u64> seen;
        for (std::string line; std::getline(in, line);) {
            const u64 h = std::strtoull(line.c_str(), nullptr, 16);
            ordered_.insert(h);
            if (files.count(h) && seen.insert(h).second)
                order.push_back(h);
        }
        for (auto& [h, p] : files)
            if (seen.insert(h).second)
                order.push_back(h);
    }
    for (const u64 hash : order) {
        const Prog& prog = files[hash];
        const shader::Stage stage = prog.stage == 0   ? shader::Stage::Vertex
                                    : prog.stage == 1 ? shader::Stage::Fragment
                                                      : shader::Stage::Compute;
        if (shaders_.find(hash) != shaders_.end() || HasFailed(hash))
            continue;
        std::vector<u8> code;
        if (!PackGet(PackKind(PackProgram + prog.stage), hash, code)) {
            code = common::ReadFile(prog.path);
            if (code.empty())
                continue;
            PackPut(PackKind(PackProgram + prog.stage), hash, code); // folded in for the next launch
        }
        if (stage == shader::Stage::Compute) { // needs the launch options recorded next to it
            std::vector<u8> o;
            if (!PackGet(PackOpt, hash, o)) {
                o = common::ReadFile(dump_dir_ / fmt::format("{:016x}.cs.opt", hash));
                if (o.size() >= 16)
                    PackPut(PackOpt, hash, o);
            }
            if (o.size() < 16)
                continue;
            shader::TranslateOptions opt;
            u32 shape[4];
            std::memcpy(shape, o.data(), 16);
            opt.block[0] = shape[0], opt.block[1] = shape[1], opt.block[2] = shape[2], opt.shared_bytes = shape[3];
            opt.cbuf1 = o.size() > 16 ? o.data() + 16 : nullptr; // copied by Queue
            opt.cbuf1_size = u32(o.size() - 16);
            Queue(shader::Stage::Compute, hash, code.data(), code.size(), &opt, true);
        } else {
            Queue(stage, hash, code.data(), code.size(), nullptr, true);
        }
        n++;
    }
    std::ifstream in(dump_dir_ / "variants.txt");
    std::unordered_set<std::string> seen;
    for (std::string line; std::getline(in, line);) {
        VariantKey v;
        unsigned long long base = 0;
        if (seen.insert(line).second &&
            std::sscanf(line.c_str(), "%llx %x %x %x %x %x %x", &base, &v.attr_uint, &v.attr_sint, &v.attr_scaled,
                        &v.attr_snorm10, &v.tex_uint, &v.tex_sint) == 7)
            prewarm_variants_.emplace_back(base, v);
    }
    prewarm_reported_ = false;
    prewarm_total = u32(n + prewarm_variants_.size());
    LOG_INFO("Shader", "prewarm: compiling {} programs and {} variants seen in earlier runs", n,
             prewarm_variants_.size());
}

void ShaderCache::Tick() {
    if (rescan_.exchange(false)) // new programs in the manifest (first-launch harvest)
        Prewarm();
    const u32 now = ++frame;
    Anticipate();
    if (now % 600 == 0) { // every ~5 s: release driver objects no draw has bound for a minute
        u32 n = 0;
        for (auto& [hash, sp] : shaders_) {
            HostShader& s = *sp;
            if (!s.created || s.state != HostShader::Ready || s.dxbc.empty() || now - s.last_used < 3600 ||
                s.stage == shader::Stage::Compute)
                continue;
            s.vs.Reset();
            s.ps.Reset();
            s.created = false;
            n++;
        }
        released += n;
        live_objects -= n;
        if (now % 6000 == 0 || n)
            LOG_INFO("Shader", "shader objects: {} live, {} created by draws so far, {} released as unused", live_objects,
                     realized.load(), released);
    }
    if (prewarm_reported_)
        return;
    replaying_ = true;
    std::erase_if(prewarm_variants_, [&](const std::pair<u64, VariantKey>& pv) {
        auto it = shaders_.find(pv.first);
        if (it == shaders_.end())
            return true;
        HostShader* base = it->second.get();
        if (base->state == HostShader::Pending)
            return false;
        GetVariant(base, pv.second); // no-op for failed bases
        return true;
    });
    replaying_ = false;
    if (prewarm_variants_.empty() && pending == 0) {
        prewarm_reported_ = true;
        const double s = double(prewarm_end_ns.load() - prewarm_start_.time_since_epoch().count()) / 1e9;
        LOG_INFO("Shader", "prewarm complete: {} shaders ready after {:.1f} s ({} failed); {} from the bytecode cache, "
                           "{} skipped translation; translation {:.1f} s, compilation {:.1f} s (driver shader creation "
                           "{:.1f} s) of worker time",
                 shaders_.size(), s, failures.load(), cache_hits.load(), info_hits.load(), translate_ns / 1e9,
                 compile_ns / 1e9, create_ns / 1e9);
    }
}

} // namespace gpu

namespace gpu {

int BuildShadersAhead(const fs::path& game_dir) {
    const auto t0 = std::chrono::steady_clock::now();
    auto say = [](const std::string& s) {
        std::fputs((s + "\n").c_str(), stdout);
        std::fflush(stdout);
    };
    const fs::path dump = "shader_dump";
    std::error_code ec;
    fs::create_directories(dump, ec);
    // 1. vertex and pixel programs: every one the game ships (the same harvest a first launch would do)
    if (const fs::path shaders = game_dir / "romfs" / "SHADER", marker = dump / "harvested.txt"; !fs::exists(marker)) {
        if (!fs::exists(shaders)) {
            say("no romfs/SHADER in " + game_dir.string());
            return 1;
        }
        say("Finding the game's shader programs...");
        const size_t n = shader::HarvestPrograms(shaders, dump);
        std::ofstream(marker) << n << "\n";
        say(fmt::format("{} vertex/pixel programs found in the game data", n));
    }
    // 2. programs the harvest cannot find (all compute programs, a few vertex/pixel ones): at the locations
    // learned from play, read from this install
    size_t cs_added = 0, cs_known = 0, cs_skipped = 0;
    {
        std::vector<u8> main_image;
        if (auto img = loader::LoadNso(common::ReadFile(game_dir / "exefs" / "main")))
            main_image = std::move(img->image);
        std::unordered_map<u16, std::vector<u8>> files;
        for (const auto& k : shader::KnownPrograms()) {
            if (!k.hash || k.stage > 2)
                continue;
            cs_known++;
            const bool compute = k.stage == 2;
            const fs::path bin = dump / fmt::format("{:016x}.{}.bin", k.hash, k.stage == 0 ? "vs" : compute ? "cs" : "ps");
            const fs::path opt_path = dump / fmt::format("{:016x}.cs.opt", k.hash);
            if (fs::exists(bin) && (!compute || fs::exists(opt_path)))
                continue;
            const std::vector<u8>* src = &main_image;
            if (k.file) {
                if (k.file >= shader::KnownFiles().size())
                    continue;
                auto& f = files[k.file];
                if (f.empty())
                    f = common::ReadFile(game_dir / "romfs" / fs::u8path(shader::KnownFiles()[k.file]));
                src = &f;
            }
            if (u64(k.offset) + k.size > src->size()) {
                cs_skipped++;
                continue;
            }
            if (!compute) {
                const u8* code = src->data() + k.offset;
                if ((Hash64(code, k.size) ^ (u64(k.stage) << 60)) != k.hash) {
                    cs_skipped++;
                    continue;
                }
                common::WriteFile(bin, std::span(code, k.size));
                cs_added++;
                continue;
            }
            std::vector<u8> code(0x50 + k.size, 0); // behind a zeroed SPH, as the renderer hands them over
            std::memcpy(code.data() + 0x50, src->data() + k.offset, k.size);
            const u32 shape[4] = {k.block[0], k.block[1], k.block[2], k.shared_bytes};
            const u64 hash = (Hash64(code.data(), code.size()) ^ (u64(shader::Stage::Compute) << 60)) ^
                             Hash64(shape, 16) * 0x9E3779B97F4A7C15ull;
            if (hash != k.hash) { // another game version, or the copy differs: leave it to the game
                cs_skipped++;
                continue;
            }
            std::vector<u8> opt(16);
            std::memcpy(opt.data(), shape, 16);
            if (k.cb_size == shader::kUnknownCbuf) {
                // Constant buffer 1 was never recorded: fine unless the program reads jump tables from it (BRX).
                bool brx = false;
                for (size_t off = 0x50 + 8; off + 8 <= code.size() && !brx; off += 8) {
                    if ((off - 0x50) % 32 == 0)
                        continue;
                    u64 insn;
                    std::memcpy(&insn, &code[off], 8);
                    brx = shader::Decode(insn) == shader::Op::BRX;
                }
                if (brx) {
                    cs_skipped++;
                    continue;
                }
            } else if (k.cb_size) {
                if (u64(k.cb_offset) + k.cb_size > main_image.size() ||
                    Hash64(main_image.data() + k.cb_offset, k.cb_size) != k.cb_hash) {
                    cs_skipped++;
                    continue;
                }
                opt.insert(opt.end(), main_image.begin() + k.cb_offset, main_image.begin() + k.cb_offset + k.cb_size);
            }
            common::WriteFile(bin, std::span(code.data(), code.size()));
            common::WriteFile(opt_path, std::span(opt.data(), opt.size()));
            cs_added++;
        }
    }
    // 3. integer variants
    size_t var_added = 0;
    {
        std::unordered_set<std::string> have;
        {
            std::ifstream in(dump / "variants.txt");
            for (std::string line; std::getline(in, line);)
                have.insert(line);
        }
        std::ofstream out(dump / "variants.txt", std::ios::app);
        for (const auto& v : shader::KnownVariants()) {
            if (!v.base)
                continue;
            const std::string line = fmt::format("{:016x} {:x} {:x} {:x} {:x} {:x} {:x}", v.base, v.key[0], v.key[1],
                                                 v.key[2], v.key[3], v.key[4], v.key[5]);
            if (have.insert(line).second) {
                out << line << "\n";
                var_added++;
            }
        }
    }
    say(fmt::format("{} more programs known from play ({} added from this install, {} could not be used); {} variants added",
                    cs_known, cs_added, cs_skipped, var_added));
    // 4. translate and compile all of it into the pack
    ComPtr<ID3D11Device> dev;
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    const bool hardware = SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &fl, 1,
                                                      D3D11_SDK_VERSION, &dev, nullptr, nullptr));
    if (!hardware && FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, &fl, 1, D3D11_SDK_VERSION,
                                              &dev, nullptr, nullptr))) {
        say("could not create a Direct3D 11 device");
        return 1;
    }
    u32 total = 0, failed = 0;
    size_t count = 0;
    {
        // Never destroyed: its out-of-process compile workers are detached threads (the process exits right after).
        // One worker of each kind per CPU this process may run on (the setup can limit it with an affinity mask).
        DWORD_PTR mask = 0, sys = 0;
        unsigned cpus = GetProcessAffinityMask(GetCurrentProcess(), &mask, &sys) ? unsigned(std::popcount(u64(mask))) : 0;
        if (!cpus)
            cpus = std::max(2u, std::thread::hardware_concurrency());
        ShaderCache& cache = *new ShaderCache(dev.Get(), cpus);
        cache.Prewarm();
        total = cache.prewarm_total;
        say(fmt::format("Compiling {} shaders on {} CPUs...", total, cpus));
        auto last = std::chrono::steady_clock::now();
        while (!cache.PrewarmDone()) {
            cache.Tick();
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            if (std::chrono::steady_clock::now() - last > std::chrono::seconds(10)) {
                last = std::chrono::steady_clock::now();
                say(fmt::format("  {} of {} left", cache.pending.load(), total));
            }
        }
        failed = cache.failures;
        count = cache.Count();
        const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        say(fmt::format("Shaders ready: {} programs and variants ({} prepared, {} cannot be translated) in {:.0f} s",
                        count, total, failed, s));
        // 5. the graphics driver's own shader cache. The driver turns this bytecode into GPU code at a shader's first
        // use (~0.1 s each cold, several seconds for a new area's shaders) and keeps the result on disk: creating
        // every object once here, and waiting for its compiler threads, makes the first launch use warm entries.
        if (hardware && !std::getenv("MUA3_NO_DRIVER_WARM")) {
            const auto w0 = std::chrono::steady_clock::now();
            const u32 n = cache.WarmDriverCache();
            say(fmt::format("Preparing {} shaders in the graphics driver...", n));
            const double secs = WaitForDriverCompiles(std::chrono::minutes(20));
            say(fmt::format("Graphics driver shaders prepared in {:.0f} s ({:.0f} s after the last one was handed over)",
                            std::chrono::duration<double>(std::chrono::steady_clock::now() - w0).count(), secs));
            SaveDriverVersion(dev.Get());
        }
    }
    return 0;
}

} // namespace gpu
