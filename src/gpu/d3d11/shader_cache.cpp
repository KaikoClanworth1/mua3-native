#include <cstring>
#include <chrono>
#include <unordered_set>
#include <fstream>
#include "gpu/d3d11/shader_cache.h"
#include "gpu/dxbc/dxbc.h"

#include <cctype>
#include <cstdlib>
#include <d3dcompiler.h>
#include <windows.h>

#pragma comment(lib, "d3dcompiler.lib")

namespace gpu {

// Bump kTranslatorVersion whenever the translator output changes, to invalidate cached bytecode.
constexpr int kTranslatorVersion = 29;


namespace {
const char* StageTag(shader::Stage st) {
    return st == shader::Stage::Vertex ? "vs" : st == shader::Stage::Compute ? "cs" : "ps";
}
const char* StageTarget(shader::Stage st) {
    return st == shader::Stage::Vertex ? "vs_5_0" : st == shader::Stage::Compute ? "cs_5_0" : "ps_5_0";
}
} // namespace


ShaderCache::ShaderCache(ID3D11Device* dev) : dev_(dev) {
    std::error_code ec;
    fs::create_directories(dump_dir_, ec);
    fs::create_directories(cache_dir_, ec);
    const unsigned n = std::clamp(std::thread::hardware_concurrency() / 2, 2u, 6u);
    for (unsigned i = 0; i < n; i++)
        workers_.emplace_back([this] { Worker(false); });
    workers_.emplace_back([this] { Worker(false, true); });
    // Loop shaders compile out of process (FXC can run for minutes on them); two workers so one slow shader
    // does not hold up the rest, and one more for the shaders the game is waiting on.
    for (int i = 0; i < 2; i++)
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
    // Vertex and pixel shaders with cached bytecode: the renderer's view of them comes from the cache too.
    if (!s.is_variant && !s.translated && !s.no_cache && s.stage != shader::Stage::Compute &&
        fs::exists(CachePath(s, StageTarget(s.stage))) && LoadInfo(InfoPath(s), s.info)) {
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
            SaveInfo(InfoPath(s), s.info);
    }
    const char* target = StageTarget(s.stage);
    // Compute shaders go through the direct DXBC backend (milliseconds): no slow FXC queue for them.
    const bool direct = s.stage == shader::Stage::Compute && !std::getenv("MUA3_FXC_COMPUTE");
    if (!slow && !direct && s.info.ok && s.info.uses_loop && !fs::exists(CachePath(s, target))) {
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
    s.ok = err.empty() && (s.vs || s.ps || s.cs);
    if (!s.ok) {
        failures++;
        if (!s.is_variant && !s.no_cache) // don't retry it at every launch's prewarm
            std::ofstream(cache_dir_ / fmt::format("{:016x}_v{}.fail", s.hash, kTranslatorVersion));
        common::WriteFile(dump_dir_ / fmt::format("{:016x}.{}.hlsl", s.hash, tag),
                          std::span(reinterpret_cast<const u8*>(s.info.hlsl.data()), s.info.hlsl.size()));
        LOG_ERROR("Shader", "{} {:016x} failed: {}", tag, s.hash, err.substr(0, 1500));
    } else {
        std::string un;
        for (auto& u : s.info.unimplemented)
            un += u + " ";
        LOG_INFO("Shader", "{} {:016x}: {} bytes, {} textures{}{}", tag, s.hash, s.code.size(), s.info.textures.size(),
                 un.empty() ? "" : ", unimplemented: ", un);
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

bool ShaderCache::SaveInfo(const fs::path& path, const shader::TranslateResult& info) {
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
    return common::WriteFile(path, w.out);
}

bool ShaderCache::LoadInfo(const fs::path& path, shader::TranslateResult& info) {
    const auto data = common::ReadFile(path);
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

// A cached base shader skipped translation: its variants are rewritten from the HLSL, so translate it now.
void ShaderCache::EnsureHlsl(HostShader& base) {
    if (!base.info.hlsl.empty() || base.is_variant)
        return;
    const auto code = common::ReadFile(dump_dir_ / fmt::format("{:016x}.{}.bin", base.hash, StageTag(base.stage)));
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
        if (fs::exists(ovr)) {
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
    std::vector<u8> blob = s.no_cache ? std::vector<u8>() : common::ReadFile(cached);
    if (!blob.empty())
        cache_hits++;
    // Debug aid: shader_override/<hash>.<vs|ps>.hlsl replaces the translated HLSL (never cached).
    std::string source = hlsl;
    const fs::path override_path =
        fs::path("shader_override") / fmt::format("{:016x}.{}.hlsl", s.hash, StageTag(s.stage));
    if (fs::exists(override_path)) {
        const auto text = common::ReadFile(override_path);
        source.assign(text.begin(), text.end());
        blob.clear();
        cached.clear();
        LOG_INFO("Shader", "using override {}", override_path.string());
    }
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
                if (!cached.empty())
                    common::WriteFile(cached, blob);
            } else {
                const auto e = common::ReadFile(out.string() + ".err");
                err.assign(e.begin(), e.end());
            }
        }
        std::error_code ec;
        fs::remove(in, ec);
        fs::remove(out, ec);
        fs::remove(out.string() + ".err", ec);
        if (!err.empty())
            return false;
    }
    if (blob.empty()) {
        ComPtr<ID3DBlob> code, errors;
        const UINT flags = D3DCOMPILE_OPTIMIZATION_LEVEL1 | D3DCOMPILE_IEEE_STRICTNESS;
        const HRESULT hr = D3DCompile(source.data(), source.size(), nullptr, nullptr, nullptr, "main", target, flags, 0,
                                      &code, &errors);
        if (FAILED(hr)) {
            err = errors ? std::string((const char*)errors->GetBufferPointer(), errors->GetBufferSize()) : "D3DCompile failed";
            return false;
        }
        blob.assign((const u8*)code->GetBufferPointer(), (const u8*)code->GetBufferPointer() + code->GetBufferSize());
        if (!cached.empty())
            common::WriteFile(cached, blob);
    }
    HRESULT hr;
    const auto k0 = std::chrono::steady_clock::now(); // the driver builds its native code here
    if (s.stage == shader::Stage::Vertex) {
        hr = dev_->CreateVertexShader(blob.data(), blob.size(), nullptr, &s.vs);
        s.dxbc = blob;
    }
    else if (s.stage == shader::Stage::Compute)
        hr = dev_->CreateComputeShader(blob.data(), blob.size(), nullptr, &s.cs);
    else
        hr = dev_->CreatePixelShader(blob.data(), blob.size(), nullptr, &s.ps);
    create_ns += u64((std::chrono::steady_clock::now() - k0).count());
    if (FAILED(hr)) {
        err = fmt::format("Create shader failed {:#x}", u32(hr));
        return false;
    }
    return true;
}

HostShader* ShaderCache::Get(shader::Stage stage, const u8* code, size_t size, const shader::TranslateOptions* opt) {
    size = shader::ProgramSize(code, size);
    u64 hash = Hash64(code, size) ^ (u64(stage) << 60);
    if (opt) { // the launch shape is compiled into compute shaders
        const u32 shape[4] = {opt->block[0], opt->block[1], opt->block[2], opt->shared_bytes};
        hash ^= Hash64(shape, sizeof(shape)) * 0x9E3779B97F4A7C15ull;
    }
    if (ordered_.insert(hash).second) // first use this launch and never listed: the next Prewarm starts with it
        std::ofstream(dump_dir_ / "order.txt", std::ios::app) << fmt::format("{:016x}\n", hash);
    auto it = shaders_.find(hash);
    if (it != shaders_.end()) {
        HostShader* found = it->second.get();
        if (found->prewarm && found->state == HostShader::Pending) { // still waiting in the prewarm backlog
            found->prewarm = false;
            std::lock_guard lk(mutex_);
            // already translated and handed to the slow (loop shader) queue, or still in the normal one
            (found->translated && found->info.uses_loop ? slow_queue_ : queue_).push_front(found);
            cv_.notify_all();
        }
        return found;
    }
    common::WriteFile(dump_dir_ / fmt::format("{:016x}.{}.bin", hash, StageTag(stage)), std::span(code, size));
    if (opt) { // compute launch shape and constant buffer 1, so Prewarm can rebuild the kernel exactly
        std::vector<u8> o(16 + opt->cbuf1_size);
        const u32 shape[4] = {opt->block[0], opt->block[1], opt->block[2], opt->shared_bytes};
        std::memcpy(o.data(), shape, 16);
        if (opt->cbuf1_size)
            std::memcpy(o.data() + 16, opt->cbuf1, opt->cbuf1_size);
        common::WriteFile(dump_dir_ / fmt::format("{:016x}.cs.opt", hash), std::span(o.data(), o.size()));
    }
    return Queue(stage, hash, code, size, opt, false);
}

HostShader* ShaderCache::Queue(shader::Stage stage, u64 hash, const u8* code, size_t size,
                               const shader::TranslateOptions* opt, bool prewarm) {
    auto s = std::make_unique<HostShader>();
    s->prewarm = prewarm;
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
    LOG_INFO("Shader", "variant {:016x} of {:016x} (tex uint {:#x} sint {:#x})", key, base->hash, v.tex_uint, v.tex_sint);
    if (!replaying_) { // newly seen (not replayed from the manifest): remember it for the next prewarm
        std::ofstream(dump_dir_ / "variants.txt", std::ios::app)
            << fmt::format("{:016x} {:x} {:x} {:x} {:x} {:x} {:x}\n", base->hash, v.attr_uint, v.attr_sint,
                           v.attr_scaled, v.attr_snorm10, v.tex_uint, v.tex_sint);
    }
    EnsureHlsl(*base);
    s->info = base->info;
    std::string& src = s->info.hlsl;
    // Debug overrides of the base shader apply to its variants too.
    const fs::path override_path = fs::path("shader_override") /
        fmt::format("{:016x}.{}.hlsl", base->hash, base->stage == shader::Stage::Vertex ? "vs" : "ps");
    if (fs::exists(override_path)) {
        const auto text = common::ReadFile(override_path);
        src.assign(text.begin(), text.end());
        s->no_cache = true;
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
    std::unordered_map<u64, fs::path> files;
    std::vector<u64> order;
    for (const auto& e : fs::directory_iterator(dump_dir_, ec)) {
        const std::string name = e.path().filename().string(); // <16 hex>.<vs|ps>.bin
        if (name.size() != 23 || !name.ends_with(".bin"))
            continue;
        const std::string tag = name.substr(17, 2);
        if (tag != "vs" && tag != "ps" && tag != "cs")
            continue;
        files[std::strtoull(name.substr(0, 16).c_str(), nullptr, 16)] = e.path();
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
        const fs::path& path = files[hash];
        const std::string name = path.filename().string();
        const std::string tag = name.substr(17, 2);
        if (shaders_.find(hash) != shaders_.end() ||
            fs::exists(cache_dir_ / fmt::format("{:016x}_v{}.fail", hash, kTranslatorVersion)))
            continue;
        const auto code = common::ReadFile(path);
        if (code.empty())
            continue;
        if (tag == "cs") { // needs the launch options recorded next to it
            const auto o = common::ReadFile(dump_dir_ / (name.substr(0, 16) + ".cs.opt"));
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
            Queue(tag == "vs" ? shader::Stage::Vertex : shader::Stage::Fragment, hash, code.data(), code.size(), nullptr,
                  true);
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
