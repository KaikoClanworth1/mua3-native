#pragma once
#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>

#include <d3d11.h>
#include <wrl/client.h>

#include "gpu/shader/translator.h"

namespace gpu {

using Microsoft::WRL::ComPtr;

struct HostShader {
    enum State { Pending, Ready, Failed };
    shader::Stage stage{};
    u64 hash = 0;
    std::atomic<int> state{Pending};
    bool ok = false; // valid once state != Pending
    shader::TranslateResult info;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    std::vector<u8> code; // guest program (kept until compiled)
    std::vector<u8> dxbc; // vertex shaders keep their bytecode for input layouts
};

// Translates and compiles shaders on background threads so the game never stalls on FXC.
class ShaderCache {
public:
    explicit ShaderCache(ID3D11Device* dev);
    ~ShaderCache();
    // `code` = guest program starting at its SPH, `size` bytes available.
    HostShader* Get(shader::Stage stage, const u8* code, size_t size);
    size_t Count() const { return shaders_.size(); }
    std::atomic<u32> failures{0};
    std::atomic<u32> pending{0};

private:
    void Worker();
    void Build(HostShader& s);
    bool Compile(HostShader& s, const std::string& hlsl, std::string& err);

    ID3D11Device* dev_;
    std::unordered_map<u64, std::unique_ptr<HostShader>> shaders_;
    fs::path dump_dir_ = "shader_dump";
    fs::path cache_dir_ = "shader_cache";

    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<HostShader*> queue_;
    std::vector<std::thread> workers_;
    bool stop_ = false;
};

u64 Hash64(const void* data, size_t size);

} // namespace gpu
