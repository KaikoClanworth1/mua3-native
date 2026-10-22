#include "gpu/d3d11/shader_cache.h"

#include <d3dcompiler.h>
#include <windows.h>

#pragma comment(lib, "d3dcompiler.lib")

namespace gpu {

u64 Hash64(const void* data, size_t size) {
    // FNV-1a 64 over 8-byte words (fast enough for shader programs).
    const u8* p = static_cast<const u8*>(data);
    u64 h = 0xcbf29ce484222325ull;
    size_t i = 0;
    for (; i + 8 <= size; i += 8) {
        u64 w;
        std::memcpy(&w, p + i, 8);
        h ^= w;
        h *= 0x100000001b3ull;
        h ^= h >> 29;
    }
    for (; i < size; i++) {
        h ^= p[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

ShaderCache::ShaderCache(ID3D11Device* dev) : dev_(dev) {
    std::error_code ec;
    fs::create_directories(dump_dir_, ec);
    fs::create_directories(cache_dir_, ec);
    const unsigned n = std::clamp(std::thread::hardware_concurrency() / 2, 2u, 6u);
    for (unsigned i = 0; i < n; i++)
        workers_.emplace_back([this] { Worker(); });
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

void ShaderCache::Worker() {
    SetThreadDescription(GetCurrentThread(), L"ShaderCompiler");
    for (;;) {
        HostShader* s;
        {
            std::unique_lock lk(mutex_);
            cv_.wait(lk, [&] { return stop_ || !queue_.empty(); });
            if (stop_)
                return;
            s = queue_.front();
            queue_.pop_front();
        }
        Build(*s);
        pending--;
    }
}

void ShaderCache::Build(HostShader& s) {
    const char* tag = s.stage == shader::Stage::Vertex ? "vs" : "ps";
    s.info = shader::Translate(s.stage, s.code.data(), s.code.size());
    std::string err;
    if (s.info.ok) {
        if (!Compile(s, s.info.hlsl, err) && s.info.uses_loop) {
            // Gradient ops inside loops are rejected by FXC; retry with explicit LOD 0.
            shader::TranslateOptions opt;
            opt.force_lod0 = true;
            s.info = shader::Translate(s.stage, s.code.data(), s.code.size(), opt);
            err.clear();
            Compile(s, s.info.hlsl, err);
        }
    } else {
        err = s.info.error;
    }
    s.ok = err.empty() && (s.vs || s.ps);
    if (!s.ok) {
        failures++;
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
}

bool ShaderCache::Compile(HostShader& s, const std::string& hlsl, std::string& err) {
    const char* target = s.stage == shader::Stage::Vertex ? "vs_5_0" : "ps_5_0";
    // Bump kTranslatorVersion whenever the translator output changes, to invalidate cached bytecode.
    constexpr int kTranslatorVersion = 7;
    const fs::path cached = cache_dir_ / fmt::format("{:016x}_{}_v{}.dxbc", s.hash, target, kTranslatorVersion);
    std::vector<u8> blob = common::ReadFile(cached);
    if (blob.empty()) {
        ComPtr<ID3DBlob> code, errors;
        const HRESULT hr = D3DCompile(hlsl.data(), hlsl.size(), nullptr, nullptr, nullptr, "main", target,
                                      D3DCOMPILE_OPTIMIZATION_LEVEL1 | D3DCOMPILE_IEEE_STRICTNESS, 0, &code, &errors);
        if (FAILED(hr)) {
            err = errors ? std::string((const char*)errors->GetBufferPointer(), errors->GetBufferSize()) : "D3DCompile failed";
            return false;
        }
        blob.assign((const u8*)code->GetBufferPointer(), (const u8*)code->GetBufferPointer() + code->GetBufferSize());
        common::WriteFile(cached, blob);
    }
    HRESULT hr;
    if (s.stage == shader::Stage::Vertex) {
        hr = dev_->CreateVertexShader(blob.data(), blob.size(), nullptr, &s.vs);
        s.dxbc = blob;
    }
    else
        hr = dev_->CreatePixelShader(blob.data(), blob.size(), nullptr, &s.ps);
    if (FAILED(hr)) {
        err = fmt::format("Create shader failed {:#x}", u32(hr));
        return false;
    }
    return true;
}

HostShader* ShaderCache::Get(shader::Stage stage, const u8* code, size_t size) {
    size = shader::ProgramSize(code, size);
    const u64 hash = Hash64(code, size) ^ (u64(stage) << 60);
    auto it = shaders_.find(hash);
    if (it != shaders_.end())
        return it->second.get();

    auto s = std::make_unique<HostShader>();
    s->stage = stage;
    s->hash = hash;
    s->code.assign(code, code + size);
    const char* tag = stage == shader::Stage::Vertex ? "vs" : "ps";
    common::WriteFile(dump_dir_ / fmt::format("{:016x}.{}.bin", hash, tag), std::span(code, size));
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

} // namespace gpu
