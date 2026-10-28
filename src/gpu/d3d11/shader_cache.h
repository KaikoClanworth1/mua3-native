#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include <d3d11.h>
#include <wrl/client.h>

#include "gpu/shader/harvest.h"
#include "gpu/shader/translator.h"

namespace gpu {

using Microsoft::WRL::ComPtr;

struct HostShader {
    enum State { Pending, Ready, Failed };
    shader::Stage stage{};
    u64 hash = 0;
    std::atomic<int> state{Pending};
    std::atomic<bool> claimed{false}; // a worker took it (urgent shaders can be queued twice)
    std::atomic<bool> slow{false};    // in the out-of-process FXC queue (minutes): draws don't wait for it
    bool prewarm = false;             // queued by Prewarm (back of the queue) and not yet asked for by the game
    bool ok = false; // valid once state != Pending
    shader::TranslateResult info;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11ComputeShader> cs;
    ComPtr<ID3D11ComputeShader> cs_ref; // MUA3_DXBC_VERIFY: the same kernel compiled by FXC, for A/B checks
    bool direct_dxbc = false;           // compiled by the FXC-free backend
    shader::TranslateOptions opt; // compute: workgroup shape and shared memory from the launch
    std::vector<u8> cbuf1;        // compute: constant buffer 1 at first launch (opt.cbuf1 points here)
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
    HostShader* Get(shader::Stage stage, const u8* code, size_t size, const shader::TranslateOptions* opt = nullptr);
    // Shader re-declared to match the draw's integer data: integer vertex attributes become uint4/int4 inputs
    // (scaled ones converted to float like Maxwell's USCALED/SSCALED) and integer textures are declared with
    // integer element types. Returns `base` when nothing is an integer type.
    struct VariantKey {
        u32 attr_uint = 0, attr_sint = 0, attr_scaled = 0; // vertex attributes (scaled = convert to float)
        u32 attr_snorm10 = 0; // A2B10G10R10 SNORM (no DXGI format): fetched as UINT, sign-extended in the shader
        u32 tex_uint = 0, tex_sint = 0;                     // texture slots with integer formats
    };
    HostShader* GetVariant(HostShader* base, const VariantKey& v);
    // Queues every vertex/pixel program seen in earlier runs (shader_dump/*.bin) and, once their bases are built,
    // the integer variants recorded in shader_dump/variants.txt, so gameplay finds them compiled. Call before the
    // GPU thread starts; Tick() (GPU thread, once per frame) creates the variants and reports completion.
    void Prewarm();
    void Tick();
    void RequestRescan() { rescan_ = true; } // any thread: Tick() re-reads the manifest
    std::atomic<bool> rescan_{false};
    size_t Count() const { return shaders_.size(); }
    std::atomic<u32> failures{0};
    std::atomic<u32> pending{0};
    std::atomic<u32> prewarm_total{0}; // shaders Prewarm queued this launch (progress UI: pending of these)
    std::atomic<u64> translate_ns{0}, compile_ns{0}, create_ns{0}, cache_hits{0}, info_hits{0}; // where the time went
    // Waits up to `budget` for `s` to leave Pending. Shaders the game asks for go to the front of the queue and,
    // from the bytecode cache, are ready within milliseconds: a draw can wait for its shader instead of leaving a
    // hole in the frame. Returns true when it is no longer pending.
    bool WaitReady(HostShader* s, std::chrono::nanoseconds budget);

private:
    // urgent_only: serves only shaders the game has asked for (never the prewarm backlog), so a cold cache's
    // backlog never delays what the current frame needs.
    void Worker(bool slow, bool urgent_only = false);
    // Returns false when the shader was handed to the slow queue instead of being finished.
    bool Build(HostShader& s, bool slow);
    bool Compile(HostShader& s, const std::string& hlsl, std::string& err, bool out_of_process = false);
    fs::path CachePath(const HostShader& s, const char* target) const;
    // What the renderer needs to know about a translated shader, kept next to its bytecode so a cached vertex or
    // pixel shader is not translated again at every launch (its HLSL is only rebuilt if a variant needs it).
    fs::path InfoPath(const HostShader& s) const;
    static bool SaveInfo(const fs::path& path, const shader::TranslateResult& info);
    static bool LoadInfo(const fs::path& path, shader::TranslateResult& info);
    void EnsureHlsl(HostShader& base);

    ID3D11Device* dev_;
    common::U64Map<std::unique_ptr<HostShader>> shaders_;
    fs::path dump_dir_ = "shader_dump";
    fs::path cache_dir_ = "shader_cache";

    std::mutex mutex_;
    std::condition_variable cv_;
    std::unordered_set<u64> ordered_; // programs already listed in order.txt (first-use order for Prewarm)
    std::mutex ready_mutex_;
    std::condition_variable ready_cv_; // a shader finished (WaitReady)
    std::deque<HostShader*> queue_;
    // FXC can take minutes on some loop shaders: they compile on their own worker so nothing else waits.
    std::deque<HostShader*> slow_queue_;
    std::vector<std::thread> workers_;
    bool stop_ = false;

    HostShader* Queue(shader::Stage stage, u64 hash, const u8* code, size_t size, const shader::TranslateOptions* opt,
                      bool prewarm);
    std::vector<std::pair<u64, VariantKey>> prewarm_variants_; // waiting for their base shader
    std::chrono::steady_clock::time_point prewarm_start_;
    bool prewarm_reported_ = true;
    std::atomic<s64> prewarm_end_ns{0}; // when the last queued shader finished (the report waits for a Present)
    bool replaying_ = false; // Tick() is recreating manifest variants
};

// Compiles the HLSL file `in_path` for `target` (vs_5_0/ps_5_0) into DXBC at `out_path` (process entry point
// for `mua3 --compile-shader`). Returns false on compile errors (written to `out_path`.err).
bool CompileShaderFile(const char* in_path, const char* out_path, const char* target);

} // namespace gpu
