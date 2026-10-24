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
    // Loop shaders compile out of process (FXC can run for minutes on them); two workers so one slow shader
    // does not hold up the rest.
    for (int i = 0; i < 2; i++)
        std::thread([this] { Worker(true); }).detach();
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

void ShaderCache::Worker(bool slow) {
    SetThreadDescription(GetCurrentThread(), slow ? L"ShaderCompiler (slow)" : L"ShaderCompiler");
    auto& q = slow ? slow_queue_ : queue_;
    for (;;) {
        HostShader* s;
        {
            std::unique_lock lk(mutex_);
            cv_.wait(lk, [&] { return stop_ || !q.empty(); });
            if (stop_)
                return;
            s = q.front();
            q.pop_front();
        }
        if (Build(*s, slow))
            pending--;
    }
}

bool ShaderCache::Build(HostShader& s, bool slow) {
    const char* tag = StageTag(s.stage);
    if (!s.is_variant && !s.translated) {
        s.opt.cbuf1 = s.cbuf1.empty() ? nullptr : s.cbuf1.data();
        s.info = shader::Translate(s.stage, s.code.data(), s.code.size(), s.opt);
        if (s.info.ok && (s.info.uses_loop || s.stage == shader::Stage::Compute)) {
            // Gradient ops inside loops are rejected by FXC: loop shaders sample with explicit LOD 0.
            shader::TranslateOptions opt = s.opt;
            opt.force_lod0 = true;
            s.info = shader::Translate(s.stage, s.code.data(), s.code.size(), opt);
        }
        s.translated = true;
    }
    const char* target = StageTarget(s.stage);
    // Compute shaders go through the direct DXBC backend (milliseconds): no slow FXC queue for them.
    const bool direct = s.stage == shader::Stage::Compute && !std::getenv("MUA3_FXC_COMPUTE");
    if (!slow && !direct && s.info.ok && s.info.uses_loop && !fs::exists(CachePath(s, target))) {
        {
            std::lock_guard lk(mutex_);
            slow_queue_.push_back(&s);
        }
        cv_.notify_all();
        return false;
    }
    std::string err;
    if (s.is_variant) {
        Compile(s, s.info.hlsl, err, slow);
    } else if (s.info.ok && s.info.uses_loop && std::getenv("MUA3_NO_LOOP_SHADERS")) {
        err = "loop shaders disabled (MUA3_NO_LOOP_SHADERS)";
    } else if (s.info.ok) {
        Compile(s, s.info.hlsl, err, slow);
    } else {
        err = s.info.error;
    }
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
    s.state = s.ok ? HostShader::Ready : HostShader::Failed;
    return true;
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
    if (s.stage == shader::Stage::Vertex) {
        hr = dev_->CreateVertexShader(blob.data(), blob.size(), nullptr, &s.vs);
        s.dxbc = blob;
    }
    else if (s.stage == shader::Stage::Compute)
        hr = dev_->CreateComputeShader(blob.data(), blob.size(), nullptr, &s.cs);
    else
        hr = dev_->CreatePixelShader(blob.data(), blob.size(), nullptr, &s.ps);
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
    auto it = shaders_.find(hash);
    if (it != shaders_.end())
        return it->second.get();
    common::WriteFile(dump_dir_ / fmt::format("{:016x}.{}.bin", hash, StageTag(stage)), std::span(code, size));
    if (opt) { // compute launch shape and constant buffer 1, so Prewarm can rebuild the kernel exactly
        std::vector<u8> o(16 + opt->cbuf1_size);
        const u32 shape[4] = {opt->block[0], opt->block[1], opt->block[2], opt->shared_bytes};
        std::memcpy(o.data(), shape, 16);
        if (opt->cbuf1_size)
            std::memcpy(o.data() + 16, opt->cbuf1, opt->cbuf1_size);
        common::WriteFile(dump_dir_ / fmt::format("{:016x}.cs.opt", hash), std::span(o.data(), o.size()));
    }
    return Queue(stage, hash, code, size, opt);
}

HostShader* ShaderCache::Queue(shader::Stage stage, u64 hash, const u8* code, size_t size,
                               const shader::TranslateOptions* opt) {
    auto s = std::make_unique<HostShader>();
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
        queue_.push_back(raw);
    }
    cv_.notify_one();
    return raw;
}

HostShader* ShaderCache::GetVariant(HostShader* base, const VariantKey& v) {
    if (!base || !(v.attr_uint | v.attr_sint | v.attr_snorm10 | v.tex_uint | v.tex_sint) || base->state != HostShader::Ready)
        return base;
    const u64 key = base->hash ^ (Hash64(&v, sizeof(v)) * 0x9E3779B97F4A7C15ull);
    auto it = shaders_.find(key);
    if (it != shaders_.end())
        return it->second.get();
    auto s = std::make_unique<HostShader>();
    s->stage = base->stage;
    s->hash = key;
    s->is_variant = true;
    LOG_INFO("Shader", "variant {:016x} of {:016x} (tex uint {:#x} sint {:#x})", key, base->hash, v.tex_uint, v.tex_sint);
    if (!replaying_) { // newly seen (not replayed from the manifest): remember it for the next prewarm
        std::ofstream(dump_dir_ / "variants.txt", std::ios::app)
            << fmt::format("{:016x} {:x} {:x} {:x} {:x} {:x} {:x}\n", base->hash, v.attr_uint, v.attr_sint,
                           v.attr_scaled, v.attr_snorm10, v.tex_uint, v.tex_sint);
    }
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
        queue_.push_back(raw);
    }
    cv_.notify_one();
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
    for (const auto& e : fs::directory_iterator(dump_dir_, ec)) {
        const std::string name = e.path().filename().string(); // <16 hex>.<vs|ps>.bin
        if (name.size() != 23 || !name.ends_with(".bin"))
            continue;
        const std::string tag = name.substr(17, 2);
        if (tag != "vs" && tag != "ps" && tag != "cs")
            continue;
        const u64 hash = std::strtoull(name.substr(0, 16).c_str(), nullptr, 16);
        if (shaders_.find(hash) != shaders_.end() ||
            fs::exists(cache_dir_ / fmt::format("{:016x}_v{}.fail", hash, kTranslatorVersion)))
            continue;
        const auto code = common::ReadFile(e.path());
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
            Queue(shader::Stage::Compute, hash, code.data(), code.size(), &opt);
        } else {
            Queue(tag == "vs" ? shader::Stage::Vertex : shader::Stage::Fragment, hash, code.data(), code.size(), nullptr);
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
        const double s =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - prewarm_start_).count();
        LOG_INFO("Shader", "prewarm complete: {} shaders ready after {:.1f} s ({} failed)", shaders_.size(), s,
                 failures.load());
    }
}

} // namespace gpu
