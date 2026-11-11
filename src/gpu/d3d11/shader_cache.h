#pragma once
#include <array>
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
    // The D3D object exists. Each one costs the driver ~2.8 MB, so cached bytecode nobody is waiting for is only
    // loaded at prewarm; Draw creates the object at first use (Realize, ~0.2 ms) and objects unused for a while
    // are released again (Tick), to be recreated from the bytecode kept in `dxbc`.
    std::atomic<bool> created{true};
    u32 last_used = 0; // ShaderCache::frame when a draw last bound it (GPU thread)
    bool prewarm = false;             // queued by Prewarm (back of the queue) and not yet asked for by the game
    // Looked up because the game set it up, no draw has asked for it yet: a failure is logged at debug level only
    // (MUA3 sets up programs it never draws, some untranslatable), and as an error once a draw asks for it.
    bool quiet = false;
    std::string quiet_error;
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
    // First seen this launch: the worker writes the program to shader_dump and the pack (disk I/O stays off the GPU
    // thread; the game data and logs share a hard disk).
    bool persist = false;
    // Integer variant: built by a worker from `base`'s HLSL (re-translating the base when only its bytecode was
    // cached takes tens of milliseconds); `key` = ShaderCache::VariantKey fields; `record`: append to variants.txt.
    HostShader* base = nullptr;
    std::array<u32, 6> key{};
    bool record = false;
};

// Translates and compiles shaders on background threads so the game never stalls on FXC.
class ShaderCache {
public:
    // build_workers > 0 (the setup's shader step, nothing else running): that many in-process and that many
    // out-of-process compile workers instead of the in-game share of the CPU.
    explicit ShaderCache(ID3D11Device* dev, unsigned build_workers = 0);
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
    bool PrewarmDone() const { return prewarm_reported_; } // Tick() reported the prewarm complete
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
    // Creates the D3D object of a Ready shader whose bytecode was only loaded (created == false). GPU thread.
    void Realize(HostShader* s);
    std::atomic<u32> frame{0}; // Tick count, for HostShader::last_used
    u32 new_programs = 0, new_variants = 0; // first seen by a draw (not replayed by Prewarm): GPU thread
    u64 realize_ns = 0; // GPU thread time in Realize (driver shader creation)
    u32 realized = 0, released = 0, live_objects = 0; // driver objects created by draws / released as unused / now

private:
    // urgent_only: serves only shaders the game has asked for (never the prewarm backlog), so a cold cache's
    // backlog never delays what the current frame needs.
    void Worker(bool slow, bool urgent_only = false);
    // Returns false when the shader was handed to the slow queue instead of being finished.
    bool Build(HostShader& s, bool slow);
    bool Compile(HostShader& s, const std::string& hlsl, std::string& err, bool out_of_process = false);
    bool CreateObject(HostShader& s, const std::vector<u8>& blob, std::string& err);
    fs::path CachePath(const HostShader& s, const char* target) const;
    // What the renderer needs to know about a translated shader, kept next to its bytecode so a cached vertex or
    // pixel shader is not translated again at every launch (its HLSL is only rebuilt if a variant needs it).
    fs::path InfoPath(const HostShader& s) const;
    static std::vector<u8> EncodeInfo(const shader::TranslateResult& info);
    static bool DecodeInfo(const std::vector<u8>& data, shader::TranslateResult& info);
    // The packed cache: every cached bytecode, translation info and failure marker of this translator version
    // in one append-only file (shader_cache/pack_v<N>.bin), memory-mapped at launch. Opening thousands of small
    // files cost a warm start ~30 s of worker time; the pack costs one open. The per-shader files stay as the
    // fallback (and are folded into the pack when hit).
    // PackProgram + stage index: the guest program (what shader_dump/<hash>.<vs|ps|cs>.bin holds); PackOpt: a
    // compute program's launch options (.cs.opt).
    enum PackKind : u8 { PackDxbc = 1, PackInfo = 2, PackFail = 3, PackProgram = 4, PackOpt = 7 };
    static int StageIndex(shader::Stage s) { return s == shader::Stage::Vertex ? 0 : s == shader::Stage::Compute ? 2 : 1; }
    void OpenPack();
    bool PackGet(PackKind kind, u64 hash, std::vector<u8>& out) const;
    void PackPut(PackKind kind, u64 hash, std::span<const u8> data);
    bool HasBytecode(const HostShader& s) const;
    bool GetBytecode(const HostShader& s, std::vector<u8>& out);
    bool LoadInfoFor(const HostShader& s, shader::TranslateResult& info);
    void SaveInfoFor(const HostShader& s, const shader::TranslateResult& info);
    bool HasFailed(u64 hash) const;
    void MarkFailed(u64 hash);
    void EnsureHlsl(HostShader& base);
    void BuildVariantSource(HostShader& s); // worker: the variant's HLSL from its base's
    void AppendLine(const fs::path& file, std::string line); // any thread: appended by a background writer
    std::mutex hlsl_mutex_; // a base's HLSL (EnsureHlsl writes it; variants built on several workers copy it)

    ID3D11Device* dev_;
    common::U64Map<std::unique_ptr<HostShader>> shaders_;
    fs::path dump_dir_ = "shader_dump";
    fs::path cache_dir_ = "shader_cache";

    std::mutex mutex_;
    std::condition_variable cv_;
    std::unordered_set<u64> ordered_; // programs already listed in order.txt (first-use order for Prewarm)
    struct PackEntry {
        size_t offset = 0;
        u32 size = 0;
    };
    void* pack_file_ = nullptr; // HANDLE
    void* pack_map_ = nullptr;  // HANDLE
    const u8* pack_ = nullptr;
    size_t pack_size_ = 0;
    std::unordered_map<u64, PackEntry> pack_dxbc_, pack_info_, pack_prog_[3], pack_opt_;
    std::unordered_set<u64> pack_fail_;
    mutable std::mutex pack_mutex_; // appends (and the fail set, which grows at runtime)
    FILE* pack_out_ = nullptr;
    bool overrides_ = false; // a shader_override directory exists (debug aid): only then look for overrides
    std::mutex ready_mutex_;
    std::condition_variable ready_cv_; // a shader finished (WaitReady)
    std::deque<HostShader*> queue_;
    // FXC can take minutes on some loop shaders: they compile on their own worker so nothing else waits.
    std::deque<HostShader*> slow_queue_;
    std::vector<std::thread> workers_;
    bool stop_ = false;

    HostShader* Queue(shader::Stage stage, u64 hash, const u8* code, size_t size, const shader::TranslateOptions* opt,
                      bool prewarm, bool persist = false);
    std::vector<std::pair<u64, VariantKey>> prewarm_variants_; // waiting for their base shader
    std::chrono::steady_clock::time_point prewarm_start_;
    bool prewarm_reported_ = true;
    std::atomic<s64> prewarm_end_ns{0}; // when the last queued shader finished (the report waits for a Present)
    bool replaying_ = false; // Tick() is recreating manifest variants

    // Programs the game set up (shader/anticipate.h): looked up here, so one never seen before compiles before its
    // first draw. (The game sets up all of its ~4700 programs at boot, so this says nothing about which a level draws:
    // driver objects stay created at first use.)
    void Anticipate();                             // GPU thread (Tick)
    std::deque<u64> anticipate_pending_;           // program addresses not looked up yet
    std::unordered_map<u64, u64> anticipate_seen_; // address -> hash of the header last looked up there
    u64 anticipate_taken_ = 0, anticipate_lookups_ = 0, anticipate_new_ = 0, anticipate_logged_ = 0;
    bool anticipating_ = false; // Get() called by Anticipate: no first-use order, quiet failures
};

// Compiles the HLSL file `in_path` for `target` (vs_5_0/ps_5_0) into DXBC at `out_path` (process entry point
// for `mua3 --compile-shader`). Returns false on compile errors (written to `out_path`.err).
bool CompileShaderFile(const char* in_path, const char* out_path, const char* target);

// `mua3 --build-shaders <game dir>` (the launcher's last build step): translates and compiles every shader the game
// can use before it is first played, into shader_cache/ (current directory). The vertex and pixel programs come
// from the game's romfs/SHADER files, compute programs from the locations in known_programs (main module and
// romfs), plus the integer variants known to be drawn. Returns 0 when everything that could be built is built.
int BuildShadersAhead(const fs::path& game_dir);

} // namespace gpu
