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
    bool is_variant = false; // vertex shader re-declared for integer vertex attributes (info.hlsl is final)
    bool translated = false;
    bool no_cache = false; // built from a debug override: never read or write the bytecode cache
};

// Translates and compiles shaders on background threads so the game never stalls on FXC.
class ShaderCache {
public:
    explicit ShaderCache(ID3D11Device* dev);
    ~ShaderCache();
    // `code` = guest program starting at its SPH, `size` bytes available.
    HostShader* Get(shader::Stage stage, const u8* code, size_t size);
    // Shader re-declared to match the draw's integer data: integer vertex attributes become uint4/int4 inputs
    // (scaled ones converted to float like Maxwell's USCALED/SSCALED) and integer textures are declared with
    // integer element types. Returns `base` when nothing is an integer type.
    struct VariantKey {
        u32 attr_uint = 0, attr_sint = 0, attr_scaled = 0; // vertex attributes (scaled = convert to float)
        u32 tex_uint = 0, tex_sint = 0;                     // texture slots with integer formats
    };
    HostShader* GetVariant(HostShader* base, const VariantKey& v);
    size_t Count() const { return shaders_.size(); }
    std::atomic<u32> failures{0};
    std::atomic<u32> pending{0};

private:
    void Worker(bool slow);
    // Returns false when the shader was handed to the slow queue instead of being finished.
    bool Build(HostShader& s, bool slow);
    bool Compile(HostShader& s, const std::string& hlsl, std::string& err, bool out_of_process = false);
    fs::path CachePath(const HostShader& s, const char* target) const;

    ID3D11Device* dev_;
    std::unordered_map<u64, std::unique_ptr<HostShader>> shaders_;
    fs::path dump_dir_ = "shader_dump";
    fs::path cache_dir_ = "shader_cache";

    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<HostShader*> queue_;
    // FXC can take minutes on some loop shaders: they compile on their own worker so nothing else waits.
    std::deque<HostShader*> slow_queue_;
    std::vector<std::thread> workers_;
    bool stop_ = false;
};

u64 Hash64(const void* data, size_t size);

// Compiles the HLSL file `in_path` for `target` (vs_5_0/ps_5_0) into DXBC at `out_path` (process entry point
// for `mua3 --compile-shader`). Returns false on compile errors (written to `out_path`.err).
bool CompileShaderFile(const char* in_path, const char* out_path, const char* target);

} // namespace gpu
