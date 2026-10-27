#include "core/hle/nvn/nvn_hle.h"

#include <algorithm>
#include <intrin.h>
#include <array>
#include <chrono>
#include <bit>
#include <set>
#include <tuple>
#include <fstream>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>

#include "common/common.h"
#include "common/profiler.h"
#include "gpu/gpu.h"

namespace hle::nvn {
namespace {

using recomp::State;
using recomp::s32;
using recomp::s64;
using recomp::u32;
using recomp::u64;

void Return(State& s, u64 value) {
    s.x[0] = value;
    s.pc = s.x[30];
    s.icount++; // progress for the runtime (and the native-instruction statistics)
}

// Every NVN entry point's driver implementation, by name (filled as the game resolves them).
std::mutex g_names_mutex;
struct DriverEntry {
    recomp::Fn fn;
    u64 pc; // guest entry address (recompiled functions dispatch on s.pc)
};
std::unordered_map<std::string, DriverEntry>& Driver() {
    static std::unordered_map<std::string, DriverEntry> m;
    return m;
}
DriverEntry DriverFn(const char* name) {
    std::lock_guard lk(g_names_mutex);
    auto it = Driver().find(name);
    return it == Driver().end() ? DriverEntry{} : it->second;
}

// Calls a driver function from host code with the guest calling convention; the guest registers are restored
// afterwards. Returns false if the function left recompiled code (it then has not finished).
constexpr u64 kHostReturn = 0xFFFF'FFFF'FFFF'F000ull;
bool CallDriver(State& s, const char* name, u64& result, u64 a0, u64 a1 = 0) {
    const DriverEntry fn = DriverFn(name);
    if (!fn.fn)
        return false;
    State saved;
    std::memcpy(&saved, &s, sizeof(State));
    s.x[0] = a0;
    s.x[1] = a1;
    s.x[30] = kHostReturn;
    s.pc = fn.pc;
    fn.fn(s);
    const bool ok = s.pc == kHostReturn;
    result = s.x[0];
    const u64 icount = s.icount;
    std::memcpy(&s, &saved, sizeof(State));
    s.icount = icount;
    return ok;
}
u64 Get(State& s, const char* name, u64 object) {
    u64 r = 0;
    return CallDriver(s, name, r, object) ? r : 0;
}

// ---- pure host implementations

// NVNtextureHandle nvnDeviceGetTextureHandle(const NVNdevice*, int textureID, int samplerID): the driver packs
// both pool indices (texture in bits 0-19, sampler from bit 20) with bit 32 set.
void DeviceGetTextureHandle(State& s) {
    const u32 packed = u32(s.x[1]) | (u32(s.x[2]) << 20);
    Return(s, u64(s64(s32(packed))) | 0x100000000ull);
}

// ---- observed objects (step 2 of docs/native_renderer_plan.md): the driver still does the work; after each
// object is initialized its real properties are read back through the driver's own getters, building the
// host-side model of the game's memory pools, buffers and textures that native rendering needs.

struct PoolDesc {
    u64 size = 0, cpu = 0, gpu = 0;
    u32 flags = 0;
    u64 tex_delta = ~0ull; // texture mapping vs buffer address, learned from registered descriptors (~0: unknown)
};
struct BufferDesc {
    u64 pool = 0, offset = 0, size = 0, gpu = 0;
};
struct TextureDesc {
    u64 pool = 0, offset = 0, storage = 0;
    u64 va = 0; // texture mapping address, from its descriptor once registered (0: unknown)
    u32 flags = 0;
    u32 width = 0, height = 0, depth = 0, levels = 0, format = 0, target = 0;
};
u64 TextureVa(const PoolDesc& p, const TextureDesc& d);
struct Objects {
    std::mutex mutex;
    std::unordered_map<u64, PoolDesc> pools;
    std::unordered_map<u64, BufferDesc> buffers;
    std::unordered_map<u64, TextureDesc> textures;
    // texture/sampler pool slots: the IDs packed into texture handles (nvnDeviceGetTextureHandle)
    std::unordered_map<u32, std::pair<u64, u64>> texture_ids; // id -> (texture, texture view or 0)
    std::unordered_map<u32, u64> sampler_ids;                 // id -> sampler
    std::unordered_map<u64, u64> texture_pools;               // NVNtexturePool -> descriptor GPU address
    std::unordered_map<u32, u64> learned_formats;             // NVN format -> TIC word 0 (MUA3_NVN_LEARN)
    u64 pool_bytes = 0, texture_inits = 0;
};
Objects& Obj() {
    static Objects o;
    return o;
}

enum Wrapped {
    kMemoryPoolInitialize,
    kMemoryPoolFinalize,
    kBufferInitialize,
    kBufferFinalize,
    kTextureInitialize,
    kTextureFinalize,
    kTexturePoolRegisterTexture,
    kSamplerPoolRegisterSampler,
    kTexturePoolInitialize,
    kSamplerPoolInitialize,
    // command recording (MUA3_NVN_RECORD=1)
    kCommandBufferBeginRecording,
    kCommandBufferEndRecording,
    kQueueSubmitCommands,
    kCommandBufferCallCommands,
    kCommandBufferCopyCommands,
    kWrappedCount
};
std::array<recomp::Fn, kWrappedCount> g_driver{}; // the sdk driver's implementation of each wrapped function

// Runs the driver function; returns false if it left recompiled code mid-way (the call is then not observed).
bool Through(State& s, Wrapped w, u64& object, u64& result) {
    object = s.x[0];
    const u64 ret = s.x[30];
    g_driver[w](s);
    result = s.x[0];
    return s.pc == ret;
}

void MemoryPoolInitialize(State& s) {
    u64 pool, ok;
    if (!Through(s, kMemoryPoolInitialize, pool, ok) || !(ok & 1))
        return;
    const PoolDesc d{Get(s, "nvnMemoryPoolGetSize", pool), Get(s, "nvnMemoryPoolMap", pool),
                     Get(s, "nvnMemoryPoolGetBufferAddress", pool), u32(Get(s, "nvnMemoryPoolGetFlags", pool))};
    auto& o = Obj();
    std::lock_guard lk(o.mutex);
    o.pools[pool] = d;
    o.pool_bytes += d.size;
    LOG_INFO("NVN", "memory pool {:#x}: {:#x} bytes, cpu {:#x} gpu {:#x} flags {:#x} ({} pools, {} MB)", pool, d.size,
             d.cpu, d.gpu, d.flags, o.pools.size(), o.pool_bytes >> 20);
}
void MemoryPoolFinalize(State& s) {
    u64 pool, r;
    if (!Through(s, kMemoryPoolFinalize, pool, r))
        return;
    auto& o = Obj();
    std::lock_guard lk(o.mutex);
    if (auto it = o.pools.find(pool); it != o.pools.end()) {
        o.pool_bytes -= it->second.size;
        o.pools.erase(it);
    }
}
void BufferInitialize(State& s) {
    u64 buffer, ok;
    if (!Through(s, kBufferInitialize, buffer, ok) || !(ok & 1))
        return;
    const BufferDesc d{Get(s, "nvnBufferGetMemoryPool", buffer), Get(s, "nvnBufferGetMemoryOffset", buffer),
                       Get(s, "nvnBufferGetSize", buffer), Get(s, "nvnBufferGetAddress", buffer)};
    auto& o = Obj();
    std::lock_guard lk(o.mutex);
    o.buffers[buffer] = d;
}
void BufferFinalize(State& s) {
    u64 buffer, r;
    if (!Through(s, kBufferFinalize, buffer, r))
        return;
    auto& o = Obj();
    std::lock_guard lk(o.mutex);
    o.buffers.erase(buffer);
}
TextureDesc DescribeTexture(State& s, u64 tex) {
    TextureDesc d;
    d.pool = Get(s, "nvnTextureGetMemoryPool", tex);
    d.offset = Get(s, "nvnTextureGetMemoryOffset", tex);
    d.storage = Get(s, "nvnTextureGetStorageSize", tex);
    d.width = u32(Get(s, "nvnTextureGetWidth", tex));
    d.height = u32(Get(s, "nvnTextureGetHeight", tex));
    d.depth = u32(Get(s, "nvnTextureGetDepth", tex));
    d.levels = u32(Get(s, "nvnTextureGetLevels", tex));
    d.format = u32(Get(s, "nvnTextureGetFormat", tex));
    d.target = u32(Get(s, "nvnTextureGetTarget", tex));
    d.flags = u32(Get(s, "nvnTextureGetFlags", tex));
    // the texture mapping address (pools can be mapped several times, by memory kind)
    d.va = Get(s, "nvnTextureGetTextureAddress", tex);
    return d;
}
// Textures the Initialize wrapper did not see (render targets made before the hooks, or by unobserved code) are
// described when first bound.
void EnsureTexture(State& s, u64 tex) {
    if (!tex)
        return;
    {
        auto& o = Obj();
        std::lock_guard lk(o.mutex);
        if (o.textures.count(tex))
            return;
    }
    const TextureDesc d = DescribeTexture(s, tex);
    if (!d.width)
        return;
    auto& o = Obj();
    std::lock_guard lk(o.mutex);
    o.textures[tex] = d;
    LOG_INFO("NVN", "texture {:#x} described at bind: {}x{} format {:#x} pool {:#x}+{:#x}", tex, d.width, d.height,
             d.format, d.pool, d.offset);
}
void TextureInitialize(State& s) {
    u64 tex, ok;
    if (!Through(s, kTextureInitialize, tex, ok) || !(ok & 1))
        return;
    const TextureDesc d = DescribeTexture(s, tex);
    auto& o = Obj();
    std::lock_guard lk(o.mutex);
    o.textures[tex] = d;
    if (++o.texture_inits % 500 == 1)
        LOG_INFO("NVN", "texture {}x{}x{} levels {} format {:#x} target {} storage {:#x} in pool {:#x}+{:#x} "
                        "({} live textures, {} buffers, {} pools, {} texture ids, {} sampler ids)",
                 d.width, d.height, d.depth, d.levels, d.format, d.target, d.storage, d.pool, d.offset,
                 o.textures.size(), o.buffers.size(), o.pools.size(), o.texture_ids.size(), o.sampler_ids.size());
}
void TextureFinalize(State& s) {
    u64 tex, r;
    if (!Through(s, kTextureFinalize, tex, r))
        return;
    auto& o = Obj();
    std::lock_guard lk(o.mutex);
    o.textures.erase(tex);
}

// NVNboolean nvnTexturePoolInitialize(NVNtexturePool*, const NVNmemoryPool*, ptrdiff_t offset, int numDescriptors)
void TexturePoolInitialize(State& s) {
    const u64 mem_pool = s.x[1], offset = s.x[2];
    u64 pool, ok;
    if (!Through(s, kTexturePoolInitialize, pool, ok) || !(ok & 1))
        return;
    auto& o = Obj();
    std::lock_guard lk(o.mutex);
    if (auto p = o.pools.find(mem_pool); p != o.pools.end()) {
        o.texture_pools[pool] = p->second.gpu + offset;
        LOG_INFO("NVN", "texture pool {:#x}: descriptors at {:#x}", pool, p->second.gpu + offset);
    }
}

// NVNboolean nvnSamplerPoolInitialize(NVNsamplerPool*, const NVNmemoryPool*, ptrdiff_t offset, int numDescriptors)
void SamplerPoolInitialize(State& s) {
    const u64 mem_pool = s.x[1], offset = s.x[2];
    u64 pool, ok;
    if (!Through(s, kSamplerPoolInitialize, pool, ok) || !(ok & 1))
        return;
    auto& o = Obj();
    std::lock_guard lk(o.mutex);
    if (auto p = o.pools.find(mem_pool); p != o.pools.end()) {
        o.texture_pools[pool] = p->second.gpu + offset;
        LOG_INFO("NVN", "sampler pool {:#x}: descriptors at {:#x}", pool, p->second.gpu + offset);
    }
}

// void nvnTexturePoolRegisterTexture(const NVNtexturePool*, int id, const NVNtexture*, const NVNtextureView*):
// KTGL re-registers the textures it binds every frame (~3,200 calls per frame in ST010).
void TexturePoolRegisterTexture(State& s) {
    const u32 id = u32(s.x[1]);
    const u64 tex = s.x[2], view = s.x[3];
    u64 pool, r;
    if (!Through(s, kTexturePoolRegisterTexture, pool, r))
        return;
    auto& o = Obj();
    std::lock_guard lk(o.mutex);
    o.texture_ids[id] = {tex, view};
    // Where textures of this pool are mapped: the descriptor holds the texture's address (offset by any view's base
    // layer, so the smallest difference wins).
    if (auto t = o.textures.find(tex); t != o.textures.end())
        if (auto tp = o.texture_pools.find(pool); tp != o.texture_pools.end())
            if (auto mp = o.pools.find(t->second.pool); mp != o.pools.end()) {
                u32 tic[3]{};
                if (gpu::G().ReadGpu(tp->second + u64(id) * 32, tic, sizeof(tic))) {
                    const u64 va = (u64(tic[2] & 0xFFFF) << 32) | tic[1];
                    const u64 base = mp->second.gpu + t->second.offset;
                    if (va >= base) { // (learning table: which mapping each kind of texture uses)
                        static std::set<std::tuple<u64, u32, u64>> seen;
                        const u64 mult = mp->second.size ? (va - base) / mp->second.size : 0;
                        if (seen.insert({t->second.pool, t->second.flags, mult}).second)
                            LOG_INFO("NVN", "texture mapping: pool {:#x} (flags {:#x}) texture flags {:#x} format {:#x} -> + {} x pool size (+{:#x})",
                                     t->second.pool, mp->second.flags, t->second.flags, t->second.format, mult, (va - base) % std::max<u64>(mp->second.size, 1));
                    }
                    static int mismatches = 0;
                    if (va != TextureVa(mp->second, t->second) && mismatches++ < 8) // (views with a base layer differ)
                        LOG_WARN("NVN", "texture {:#x}: descriptor address {:#x}, nvnTextureGetTextureAddress {:#x}", tex, va,
                                 t->second.va);
                    if (va >= base && va - base < (1ull << 36) && va - base < mp->second.tex_delta) {
                        if (mp->second.tex_delta == ~0ull)
                            LOG_INFO("NVN", "memory pool {:#x}: textures mapped at buffer address + {:#x}", t->second.pool, va - base);
                        mp->second.tex_delta = va - base;
                    }
                }
            }
    // MUA3_NVN_LEARN: the hardware descriptor the driver just wrote, per NVN format (the renderer maps TIC formats)
    static const bool learn = std::getenv("MUA3_NVN_LEARN") != nullptr;
    if (learn) { // (KTGL always passes a view; it rarely changes the format)
        auto t = o.textures.find(tex);
        auto p = o.texture_pools.find(pool);
        if (t != o.textures.end() && p != o.texture_pools.end() && !o.learned_formats.count(t->second.format)) {
            u32 tic[8]{};
            if (gpu::G().ReadGpu(p->second + u64(id) * 32, tic, sizeof(tic))) {
                o.learned_formats[t->second.format] = tic[0];
                LOG_INFO("Learn", "nvn format {:#x} -> tic {:08x} {:08x} ({}x{})", t->second.format, tic[0], tic[1],
                         t->second.width, t->second.height);
            }
        }
    }
}
// void nvnSamplerPoolRegisterSampler(const NVNsamplerPool*, int id, const NVNsampler*)
void SamplerPoolRegisterSampler(State& s) {
    const u32 id = u32(s.x[1]);
    const u64 sampler = s.x[2];
    u64 pool, r;
    if (!Through(s, kSamplerPoolRegisterSampler, pool, r))
        return;
    auto& o = Obj();
    std::lock_guard lk(o.mutex);
    o.sampler_ids[id] = sampler;
}

// ---- command recording (step 3, observe mode): host command lists built from the game's command buffer calls,
// keyed by the command handle EndRecording returns, and summarized when the queue submits them. This is the
// stream the native D3D11 replay will execute.
// Every nvnCommandBuffer* command and every state-object setter goes through a generic wrapper (one template
// instance per resolved name) that records the call's registers before the driver runs.
// Commands are recorded directly in their replay form: `extra` holds Call/CopyCommands' nested handles,
// ProgramSetShaders' stage data pairs and SetRenderTargets' colour textures; `raw` the memory the command points at
// (read at record time); `objects` the bound state objects' setter calls as of record time.
using Command = ReplayCommand;
using CommandList = std::shared_ptr<std::vector<Command>>;
enum class Kind { Command, Setter };
struct GenericSlot {
    std::string name;
    recomp::Fn driver = nullptr;
    Kind kind = Kind::Command;
    bool draw = false, indirect = false, dispatch = false, defaults = false, shaders = false, rts = false;
    // replay capture: x[raw_ptr] points at raw_count ? x[raw_count] : 1 elements of raw_elem bytes; `objects`: they
    // are state objects whose shadows are copied too
    int raw_ptr = -1, raw_count = -1;
    u32 raw_elem = 0;
    bool objects = false;
    bool indexed = false; // setter whose first argument selects what it sets (render target, face)
    bool host = false;    // MUA3_NVN_NATIVE: the host encoder executes it; the driver never sees it
    // texture copies: argument indices of their NVNtextureView* and NVNcopyRegion* pairs (-1 = none)
    int copy_view[2] = {-1, -1}, copy_region[2] = {-1, -1};
};
constexpr int kGenericSlots = 320;
std::array<GenericSlot, kGenericSlots> g_generic;
std::atomic<int> g_generic_count{0};
struct Recording {
    std::mutex mutex;
    std::unordered_map<u64, std::vector<Command>> open;    // command buffer -> commands since BeginRecording
    std::unordered_map<u64, CommandList> handles;          // command handle -> finished list (shared with replays)
    u64 submits = 0, cmds = 0, draws = 0, indirect = 0, dispatches = 0, unknown = 0;
    std::atomic<u64> recorded{0};
    u64 begins = 0, ends = 0, empty_ends = 0; // diagnostics
    u64 synthetic = 0; // keys for split command lists (replay flattening)
    // state objects (blend, colour, depth/stencil, polygon, multisample, channel mask, vertex attrib/stream,
    // texture views, sampler builders, programs): object -> setter calls since SetDefaults, in order (a repeated call
    // with the same first argument replaces the earlier one)
    std::unordered_map<u64, std::vector<Command>> shadow;
};
Recording& Rec() {
    static Recording r;
    return r;
}
std::atomic<std::function<void(std::vector<ReplaySpan>&&)>*> g_sink{nullptr};
extern "C" __declspec(dllimport) void* __stdcall GetCurrentThread();
std::atomic<u64> g_submit_ns{0}, g_record_cycles{0}; // recorder cost diagnostics
inline u64 NowNs() {
    return u64(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}
int ProfileAt() {
    static const int at = [] {
        const char* e = std::getenv("MUA3_PROFILE_NVN");
        return e ? std::atoi(e) : -1;
    }();
    return at;
}
// MUA3_PROFILE_NVN=<seconds>: after that long, samples the first threads that record commands (10 s each).
void MaybeProfileRecorder() {
    static const auto start = std::chrono::steady_clock::now();
    static std::atomic<int> started{0};
    thread_local bool mine = false;
    if (mine || started.load() >= 3 || std::chrono::steady_clock::now() - start < std::chrono::seconds(ProfileAt()))
        return;
    mine = true;
    const int n = started++;
    if (n < 3)
        common::StartSamplingProfiler(GetCurrentThread(), n == 0 ? "NVN recorder 0" : n == 1 ? "NVN recorder 1" : "NVN recorder 2", 10, 0);
}
template <int I>
void Generic(State& s) {
    // Test aid: MUA3_NVN_RECORD=2 installs the wrappers but records nothing (measures the wrapping alone).
    static const bool passthrough = [] {
        const char* e = std::getenv("MUA3_NVN_RECORD");
        return e && e[0] == '2';
    }();
    if (passthrough)
        return g_generic[I].driver(s);
    if (ProfileAt() >= 0)
        MaybeProfileRecorder();
    const u64 cycles0 = __rdtsc();
    const GenericSlot& g = g_generic[I];
    Command c;
    c.fn = I;
    c.name = g.name;
    for (int k = 0; k < 8; k++)
        c.x[k] = s.x[k];
    for (int k = 0; k < 4; k++)
        c.v[k] = u32(s.v[k].lo);
    if (g.rts) { // SetRenderTargets(cb, n, NVNtexture** colors, views, depth, depthView): the arrays are temporaries
        for (u64 i = 0; i < u32(s.x[1]) && i < 8; i++)
            c.extra.push_back(recomp::Rd<u64>(s, s.x[2] + i * 8));
        if (g_sink.load()) {
            for (u64 t : c.extra)
                EnsureTexture(s, t);
            EnsureTexture(s, s.x[4]);
        }
    }
    if (g.shaders) // nvnProgramSetShaders(program, count, const NVNshaderData*): {code address, control pointer}
        for (u64 i = 0; i < u32(s.x[1]) && i < 8; i++) {
            c.extra.push_back(recomp::Rd<u64>(s, s.x[2] + i * 16));
            c.extra.push_back(recomp::Rd<u64>(s, s.x[2] + i * 16 + 8));
        }
    const bool replay = g_sink.load() != nullptr;
    // copies: regions (6 ints each) into raw, views into objects (below)
    if (replay)
        for (int k = 0; k < 2; k++)
            if (g.copy_region[k] >= 0) {
                const u64 r = s.x[g.copy_region[k]];
                for (u32 o = 0; o < 24; o += 8)
                    c.raw.push_back(r ? recomp::Rd<u64>(s, r + o) : 0);
            }
    // ... followed by the views' raw bytes (5 words each; the game fills them without setters)
    if (replay)
        for (int k = 0; k < 2; k++)
            if (g.copy_view[k] >= 0) {
                const u64 v = s.x[g.copy_view[k]];
                for (u32 o = 0; o < 40; o += 8)
                    c.raw.push_back(v ? recomp::Rd<u64>(s, v + o) : 0);
            }
    u32 raw_n = 0;
    if (replay && g.raw_ptr >= 0 && s.x[g.raw_ptr]) {
        raw_n = g.raw_count >= 0 ? std::min<u32>(u32(s.x[g.raw_count]), 32) : 1;
        for (u32 o = 0; o < raw_n * g.raw_elem; o += 8)
            c.raw.push_back(recomp::Rd<u64>(s, s.x[g.raw_ptr] + o));
    }
    // Commands append to the calling thread's current command buffer without the lock (each command buffer is
    // recorded by one thread; map nodes are stable, so the cached list stays valid).
    thread_local u64 tls_cb = 0;
    thread_local std::vector<Command>* tls_list = nullptr;
    if (g.kind == Kind::Command && !g.objects) {
        if (tls_cb != s.x[0] || !tls_list) {
            auto& rec = Rec();
            std::lock_guard lk(rec.mutex);
            tls_list = &rec.open[s.x[0]];
            tls_cb = s.x[0];
        }
        tls_list->push_back(std::move(c));
        Rec().recorded.fetch_add(1, std::memory_order_relaxed);
    } else {
        auto& rec = Rec();
        std::lock_guard lk(rec.mutex);
        if (g.kind == Kind::Command) {
            if (replay && g.copy_view[0] >= 0)
                for (int k = 0; k < 2 && g.copy_view[k] >= 0; k++) {
                    auto& calls = c.objects.emplace_back();
                    if (auto it = rec.shadow.find(s.x[g.copy_view[k]]); s.x[g.copy_view[k]] && it != rec.shadow.end())
                        for (const Command& sc : it->second) {
                            StateCall& kc = calls.emplace_back();
                            kc.name = g_generic[sc.fn].name;
                            std::memcpy(kc.x, sc.x, sizeof(kc.x));
                            std::memcpy(kc.v, sc.v, sizeof(kc.v));
                        }
                }
            if (g.objects) {
                c.objects.reserve(raw_n);
                for (u32 i = 0; i < raw_n; i++) {
                    auto& calls = c.objects.emplace_back();
                    if (auto it = rec.shadow.find(s.x[g.raw_ptr] + u64(i) * g.raw_elem); it != rec.shadow.end()) {
                        calls.reserve(it->second.size());
                        for (const Command& sc : it->second) {
                            StateCall& k = calls.emplace_back();
                            k.name = g_generic[sc.fn].name;
                            std::memcpy(k.x, sc.x, sizeof(k.x));
                            std::memcpy(k.v, sc.v, sizeof(k.v));
                            k.handles = sc.extra;
                        }
                    }
                }
            }
            rec.open[s.x[0]].push_back(std::move(c));
            rec.recorded++;
        } else {
            auto& obj = rec.shadow[s.x[0]];
            if (g.defaults)
                obj.clear();
            if (obj.capacity() < 8)
                obj.reserve(8);
            auto same = std::find_if(obj.begin(), obj.end(),
                                     [&](const Command& o) { return o.fn == I && (!g.indexed || o.x[1] == c.x[1]); });
            if (same != obj.end())
                obj.erase(same);
            else if (obj.size() >= 64)
                obj.erase(obj.begin());
            obj.push_back(std::move(c));
        }
    }
    g_record_cycles.fetch_add(__rdtsc() - cycles0, std::memory_order_relaxed);
    if (g.host)
        return Return(s, 0);
    g.driver(s);
}
template <size_t... I>
constexpr std::array<recomp::Fn, sizeof...(I)> MakeGeneric(std::index_sequence<I...>) {
    return {Generic<int(I)>...};
}
const std::array<recomp::Fn, kGenericSlots> kGenericFns = MakeGeneric(std::make_index_sequence<kGenericSlots>{});

// Which unhandled NVN names get a generic recording wrapper.
bool GenericKind(std::string_view n, Kind& kind) {
    auto starts = [&](std::string_view p) { return n.substr(0, p.size()) == p; };
    if (n.find("Get") != std::string_view::npos)
        return false;
    if (starts("nvnCommandBuffer")) {
        for (std::string_view x : {"Initialize", "Finalize", "BeginRecording", "EndRecording", "CallCommands",
                                   "CopyCommands", "AddCommandMemory", "AddControlMemory", "SetMemoryCallback",
                                   "SetDebugLabel"})
            if (n.find(x) != std::string_view::npos)
                return false;
        kind = Kind::Command;
        return true;
    }
    for (std::string_view p : {"nvnBlendState", "nvnColorState", "nvnChannelMaskState", "nvnDepthStencilState",
                               "nvnPolygonState", "nvnMultisampleState", "nvnVertexAttribState",
                               "nvnVertexStreamState", "nvnTextureView", "nvnSamplerBuilder", "nvnProgramSetShaders"})
        if (starts(p) && n.find("Set") != std::string_view::npos) {
            kind = Kind::Setter;
            return true;
        }
    return false;
}
// void nvnCommandBufferCall/CopyCommands(NVNcommandBuffer*, int numCommands, const NVNcommandHandle* handles):
// a command buffer running (or copying in) other recorded command buffers.
template <Wrapped W>
void NestCommands(State& s) {
    const u64 cb = s.x[0], count = u32(s.x[1]), array = s.x[2];
    Command c;
    c.x[0] = cb, c.x[1] = count;
    for (u64 i = 0; i < count && i < 4096; i++)
        c.extra.push_back(recomp::Rd<u64>(s, array + i * 8));
    u64 obj, r;
    if (!Through(s, W, obj, r))
        return;
    auto& rec = Rec();
    std::lock_guard lk(rec.mutex);
    rec.open[cb].push_back(std::move(c));
    rec.recorded++;
}
void CommandBufferBeginRecording(State& s) {
    u64 cb, r;
    if (!Through(s, kCommandBufferBeginRecording, cb, r))
        return;
    auto& rec = Rec();
    std::lock_guard lk(rec.mutex);
    auto& open = rec.open[cb];
    open.clear();
    if (open.capacity() < 256)
        open.reserve(256);
    rec.begins++;
}
void CommandBufferEndRecording(State& s) {
    u64 cb, handle;
    if (!Through(s, kCommandBufferEndRecording, cb, handle))
        return;
    auto& rec = Rec();
    std::lock_guard lk(rec.mutex);
    if (rec.handles.size() > 4096) // handles are submitted soon after recording; drop stale ones
        rec.handles.clear();
    rec.ends++;
    rec.empty_ends += rec.open[cb].empty();
    // Handles identify command memory, which the game reuses: an empty recording must not hide a real one that
    // is still waiting to be submitted.
    // (swapped rather than moved: the command buffer keeps an allocation for its next recording)
    auto& slot = rec.handles[handle];
    auto& open = rec.open[cb];
    if (!open.empty() || !slot) {
        const size_t n = open.size();
        slot = std::make_shared<std::vector<Command>>(std::move(open));
        open = {};
        open.reserve(std::max<size_t>(n * 2, 256));
    } else
        open.clear();
}

// ---- draw resolution (front half of the native replay): walks one submitted frame in order, tracking bound state,
// and resolves each draw's resources with the learned driver conventions (docs/native_renderer_plan.md).
// Where the driver maps a pool's textures (learned against its descriptors): a COMPRESSIBLE pool (flag 0x80) is mapped
// again after its buffer mapping for plain textures (+ size) and once more for compressible ones (+ 2 * size, render
// targets); other pools use their buffer mapping.
u64 TextureVa(const PoolDesc& p, const TextureDesc& d) {
    if (d.va)
        return d.va;
    if (!(p.flags & 0x80))
        return p.gpu + d.offset;
    return p.gpu + d.offset + p.size * ((d.flags & 8) ? 2 : 1);
}
const std::unordered_map<u32, u32> kRtFormat = {{0x25, 0xD5}, {0x38, 0xD6}, {0x3D, 0xD1}, {0x3F, 0xE0},
                                                {0x0A, 0xE5}, {0x29, 0xCA}, {0x34, 0x0A}};
struct BoundStage {
    u64 ubo[16]{};      // NVN uniform buffer i (hardware cbuf i + 3)
    u64 tex[32]{};      // NVN texture handles (hardware cbuf 2, 0x20 + i * 8)
};
struct DrawState {
    BoundStage stage[6];
    u64 program[6]{}; // separable programs: BindProgram(cb, program, stage bits) binds one stage each
    u64 rt[8]{}, depth = 0;
    u32 rt_fmt[8]{}, depth_fmt = 0, num_rt = 0;
};
// NVNtexture* -> (texture GPU address, NVN format), from the object model
bool TextureVa(Objects& o, u64 tex, u64& va, u32& fmt) {
    auto t = o.textures.find(tex);
    if (t == o.textures.end())
        return false;
    auto p = o.pools.find(t->second.pool);
    if (p == o.pools.end())
        return false;
    va = TextureVa(p->second, t->second);
    fmt = t->second.format;
    return true;
}
void ResolveFrame(Recording& rec, std::ofstream& f, State& s, const std::vector<u64>& roots) {
    auto& o = Obj();
    std::lock_guard olk(o.mutex);
    DrawState st;
    size_t draws = 0, unresolved_rt = 0;
    std::vector<std::pair<u64, int>> walk;
    for (auto it = roots.rbegin(); it != roots.rend(); ++it)
        walk.emplace_back(*it, 0);
    while (!walk.empty()) {
        const auto [h, depth] = walk.back();
        walk.pop_back();
        auto it = rec.handles.find(h);
        if (it == rec.handles.end() || !it->second)
            continue;
        for (const Command& c : *it->second) {
            if (c.fn < 0) {
                for (auto n = c.extra.rbegin(); n != c.extra.rend(); ++n)
                    if (depth < 8)
                        walk.emplace_back(*n, depth + 1);
                continue;
            }
            const std::string& n = g_generic[c.fn].name;
            const u32 stage = u32(c.x[1]), idx = u32(c.x[2]);
            if (n == "nvnCommandBufferBindUniformBuffer" && stage < 6 && idx < 16)
                st.stage[stage].ubo[idx] = c.x[3];
            else if (n == "nvnCommandBufferBindTexture" && stage < 6 && idx < 32)
                st.stage[stage].tex[idx] = c.x[3];
            else if (n == "nvnCommandBufferBindProgram") {
                for (u32 b = 0; b < 6; b++)
                    if (c.x[2] & (1u << b))
                        st.program[b] = c.x[1];
            }
            else if (n == "nvnCommandBufferSetRenderTargets") {
                st.num_rt = u32(c.x[1]);
                for (u32 i = 0; i < st.num_rt && i < 8; i++) {
                    const u64 tex = i < c.extra.size() ? c.extra[i] : 0;
                    if (!TextureVa(o, tex, st.rt[i], st.rt_fmt[i]))
                        st.rt[i] = 0, unresolved_rt++;
                }
                if (!c.x[4] || !TextureVa(o, c.x[4], st.depth, st.depth_fmt))
                    st.depth = 0;
            } else if (g_generic[c.fn].draw) {
                draws++;
                std::string line = fmt::format("draw {} {}", draws, n.substr(16));
                for (u32 b = 0; b < 5; b++) {
                    if (!st.program[b])
                        continue;
                    if (auto sh = rec.shadow.find(st.program[b]); sh != rec.shadow.end())
                        for (const Command& cmd : sh->second)
                            if (g_generic[cmd.fn].shaders && !cmd.extra.empty()) {
                                u32 w[64]{}; // where the Maxwell SPH (type 1/2, version 3) starts in the blob
                                gpu::G().ReadGpu(cmd.extra[0], w, sizeof(w));
                                int sph = -1;
                                for (int k = 0; k < 64 && sph < 0; k++)
                                    if (((w[k] & 0x1F) == 1 || (w[k] & 0x1F) == 2) && ((w[k] >> 5) & 0x1F) == 3)
                                        sph = k * 4;
                                line += fmt::format(" {} {:#x}[sph+{:#x}]", "VFGCE"[b], cmd.extra[0], sph);
                            }
                }
                for (u32 i = 0; i < st.num_rt && i < 8; i++) {
                    auto hw = kRtFormat.find(st.rt_fmt[i]);
                    line += fmt::format(" rt{} {:#x}/{:#x}", i, st.rt[i], hw != kRtFormat.end() ? hw->second : 0xFFFFu);
                }
                if (st.depth)
                    line += fmt::format(" depth {:#x}", st.depth);
                for (u32 sg : {0u, 1u})
                    for (u32 i = 0; i < 16; i++)
                        if (st.stage[sg].ubo[i])
                            line += fmt::format(" s{}c{}={:#x}", sg == 1 ? 4 : 0, i + 3, st.stage[sg].ubo[i]);
                for (u32 i = 0; i < 32; i++)
                    if (st.stage[1].tex[i])
                        line += fmt::format(" t{}=tic{:#x}/tsc{:#x}", i, st.stage[1].tex[i] & 0xFFFFF,
                                            (st.stage[1].tex[i] >> 20) & 0xFFF);
                f << line << "\n";
            }
        }
    }
    f << fmt::format("# {} draws resolved, {} render targets not in the texture model\n", draws, unresolved_rt);
}
// void nvnQueueSubmitCommands(NVNqueue*, int numCommands, const NVNcommandHandle* handles)
void QueueSubmitCommands(State& s) {
    const u64 count = u32(s.x[1]), array = s.x[2];
    std::vector<u64> handles;
    for (u64 i = 0; i < count && i < 256; i++)
        handles.push_back(recomp::Rd<u64>(s, array + i * 8));
    // Observed before the driver runs: the submit makes a GPU syscall that can hand the rest of the call to the JIT.
    const u64 queue = s.x[0];
    const u64 submit_t0 = NowNs();

    std::vector<ReplaySpan> flat;
    auto* sink = g_sink.load();
    {
    auto& rec = Rec();
    std::lock_guard lk(rec.mutex);
    // MUA3_NVN_DUMP=<seconds>: write the first large submit after that time's command stream (and the state objects it binds) to nvn_frame.txt
    static const u64 dump_at = [] {
        const char* e = std::getenv("MUA3_NVN_DUMP");
        return e ? std::strtoull(e, nullptr, 10) : 0;
    }();
    static const auto t0 = std::chrono::steady_clock::now();
    static bool dumped = false;
    size_t top = 0;
    for (u64 h : handles)
        if (auto it = rec.handles.find(h); it != rec.handles.end() && it->second)
            top += it->second->size();
    if (dump_at && !dumped && top >= 100 &&
        std::chrono::steady_clock::now() - t0 > std::chrono::seconds(dump_at)) {
        dumped = true;
        std::ofstream f("nvn_frame.txt");
        std::set<u64> objects;
        std::vector<std::pair<u64, int>> walk;
        for (u64 h : handles)
            walk.emplace_back(h, 0);
        while (!walk.empty()) {
            const auto [h, depth] = walk.back();
            walk.pop_back();
            auto it = rec.handles.find(h);
            f << fmt::format("handle {:#x} ({} commands)\n", h, it == rec.handles.end() || !it->second ? 0 : it->second->size());
            if (it == rec.handles.end() || !it->second)
                continue;
            for (const Command& c : *it->second) {
                const std::string name = c.fn >= 0 ? g_generic[c.fn].name : "CallCommands";
                f << fmt::format("  {} {:x} {:x} {:x} {:x} {:x} {:x} {:x} | {} {} {} {}\n", name, c.x[1], c.x[2], c.x[3],
                                 c.x[4], c.x[5], c.x[6], c.x[7], std::bit_cast<float>(c.v[0]),
                                 std::bit_cast<float>(c.v[1]), std::bit_cast<float>(c.v[2]),
                                 std::bit_cast<float>(c.v[3]));
                if (name.find("State") != std::string::npos || name.find("BindProgram") != std::string::npos)
                    objects.insert(c.x[1]);
                if (name.find("BindVertexAttribState") != std::string::npos ||
                    name.find("BindVertexStreamState") != std::string::npos)
                    for (u64 k = 0; k < c.x[1] && k < 32; k++) // arrays of state objects: probe 4/8/16-byte strides
                        for (u64 stride : {4ull, 8ull, 16ull})
                            if (rec.shadow.count(c.x[2] + k * stride))
                                objects.insert(c.x[2] + k * stride);
                if (depth < 8)
                    for (u64 nested : c.extra)
                        walk.emplace_back(nested, depth + 1);
            }
        }
        for (u64 obj : objects) {
            auto it = rec.shadow.find(obj);
            if (it == rec.shadow.end())
                continue;
            f << fmt::format("object {:#x}\n", obj);
            for (const Command& c : it->second) {
                f << fmt::format("  {} {:x} {:x} {:x} {:x} {:x} | {} {}", g_generic[c.fn].name, c.x[1], c.x[2], c.x[3],
                                 c.x[4], c.x[5], std::bit_cast<float>(c.v[0]), std::bit_cast<float>(c.v[1]));
                for (u64 h : c.extra)
                    f << fmt::format(" {:x}", h);
                f << "\n";
            }
        }
        // texture model with GPU addresses, for learning NVN -> hardware formats (MUA3_NVN_LEARN)
        {
            auto& o = Obj();
            std::lock_guard olk(o.mutex);
            std::ofstream m("nvn_textures.txt");
            for (const auto& [tex, d] : o.textures) {
                auto p = o.pools.find(d.pool);
                if (p != o.pools.end())
                    m << fmt::format("{:#x} {} {} {} levels {} format {:#x} target {}\n", p->second.gpu + d.offset, d.width,
                                     d.height, d.depth, d.levels, d.format, d.target);
            }
        }
        g_learn_until_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now().time_since_epoch())
                               .count() +
                           60;
        {
            std::ofstream r("nvn_draws.txt");
            ResolveFrame(rec, r, s, handles);
        }
        LOG_INFO("NVN", "wrote nvn_frame.txt");
    }
    // native replay: hand this frame's commands, in execution order, to the renderer (ranges of the recorded lists;
    // nested Call/CopyCommands run at their call site)
    if (sink && *sink) {
        struct Pending {
            CommandList list;
            size_t begin;
            int depth;
        };
        std::vector<Pending> order;
        for (auto h = handles.rbegin(); h != handles.rend(); ++h)
            if (auto it = rec.handles.find(*h); it != rec.handles.end() && it->second)
                order.push_back({it->second, 0, 0});
        while (!order.empty()) {
            Pending p = std::move(order.back());
            order.pop_back();
            const auto& list = *p.list;
            size_t k = p.begin;
            while (k < list.size() && list[k].fn >= 0)
                k++;
            if (k > p.begin)
                flat.push_back({p.list, p.begin, k});
            if (k < list.size()) { // nested: the rest of this list after them
                if (k + 1 < list.size())
                    order.push_back({p.list, k + 1, p.depth});
                const auto& nested = list[k].extra;
                for (auto n = nested.rbegin(); n != nested.rend(); ++n)
                    if (auto it = rec.handles.find(*n); p.depth < 8 && it != rec.handles.end() && it->second)
                        order.push_back({it->second, 0, p.depth + 1});
            }
        }
    }
    // walk the submitted command buffers, following nested Call/CopyCommands
    std::vector<std::pair<u64, int>> work;
    for (u64 h : handles)
        work.emplace_back(h, 0);
    while (!work.empty()) {
        const auto [h, depth] = work.back();
        work.pop_back();
        auto it = rec.handles.find(h);
        if (it == rec.handles.end() || !it->second) {
            rec.unknown++;
            continue;
        }
        for (const Command& c : *it->second) {
            rec.cmds++;
            if (c.fn >= 0) {
                const GenericSlot& g = g_generic[c.fn];
                rec.draws += g.draw;
                rec.indirect += g.indirect;
                rec.dispatches += g.dispatch;
            }
            if (depth < 8 && c.fn < 0)
                for (u64 nested : c.extra)
                    work.emplace_back(nested, depth + 1);
        }
    }
    if (++rec.submits % 600 == 0) {
        LOG_INFO("NVN", "recorded stream, per submit: {:.0f} commands, {:.0f} draws ({:.0f} indirect), {:.1f} "
                        "dispatches; {} unrecorded handles (recorded {}, begins {}, ends {}, empty {}); submit {:.2f} ms, "
                        "recording {:.0f} Mcycles",
                 double(rec.cmds) / 600, double(rec.draws) / 600, double(rec.indirect) / 600,
                 double(rec.dispatches) / 600, rec.unknown, rec.recorded.load(), rec.begins, rec.ends, rec.empty_ends,
                 g_submit_ns.exchange(0) / 600e6, g_record_cycles.exchange(0) / 600e6);
        rec.cmds = rec.draws = rec.indirect = rec.dispatches = rec.unknown = 0;
    }
    }
    if (sink && *sink)
        (*sink)(std::move(flat));
    g_submit_ns.fetch_add(NowNs() - submit_t0, std::memory_order_relaxed);
    u64 q, r;
    Through(s, kQueueSubmitCommands, q, r);
}

struct Entry {
    std::string_view name;
    recomp::Fn fn;
    int wrapped = -1;    // index into g_driver for pass-through wrappers
    bool record = false; // only with MUA3_NVN_RECORD=1
};
#define NVN_ENTRY(NAME) {"nvn" #NAME, NAME, k##NAME}
const Entry kHostFunctions[] = {
    {"nvnDeviceGetTextureHandle", DeviceGetTextureHandle},
    NVN_ENTRY(MemoryPoolInitialize),
    NVN_ENTRY(MemoryPoolFinalize),
    NVN_ENTRY(BufferInitialize),
    NVN_ENTRY(BufferFinalize),
    NVN_ENTRY(TextureInitialize),
    NVN_ENTRY(TextureFinalize),
    NVN_ENTRY(TexturePoolRegisterTexture),
    NVN_ENTRY(SamplerPoolRegisterSampler),
    NVN_ENTRY(TexturePoolInitialize),
    NVN_ENTRY(SamplerPoolInitialize),
#define NVN_RECORD(NAME) {"nvn" #NAME, NAME, k##NAME, true}
    NVN_RECORD(CommandBufferBeginRecording),
    NVN_RECORD(CommandBufferEndRecording),
    NVN_RECORD(QueueSubmitCommands),
    {"nvnCommandBufferCallCommands", NestCommands<kCommandBufferCallCommands>, kCommandBufferCallCommands, true},
    {"nvnCommandBufferCopyCommands", NestCommands<kCommandBufferCopyCommands>, kCommandBufferCopyCommands, true},
#undef NVN_RECORD
};
#undef NVN_ENTRY

} // namespace

std::mutex g_resolved_mutex;
std::unordered_map<std::string, u64>& Resolved() { // every name seen, -> driver address
    static std::unordered_map<std::string, u64> m;
    return m;
}

void ResolveMissing(State& s, recomp::Fn lookup, u64 lookup_pc, u64 device,
                    const std::function<void(const char*, u64)>& found) {
    for (const auto& e : kHostFunctions) {
        const std::string name(e.name);
        {
            std::lock_guard lk(g_resolved_mutex);
            if (Resolved().count(name))
                continue;
        }
        // the name goes on the guest stack below sp (free space at a call boundary)
        const u64 str = (s.sp - 0x400) & ~u64(15);
        for (size_t i = 0; i <= name.size(); i++)
            recomp::Wr<recomp::u8>(s, str + i, i < name.size() ? recomp::u8(name[i]) : 0);
        State saved;
        std::memcpy(&saved, &s, sizeof(State));
        s.x[0] = device;
        s.x[1] = str;
        s.x[30] = kHostReturn;
        s.sp = str - 0x100;
        s.pc = lookup_pc;
        lookup(s);
        const u64 ptr = s.pc == kHostReturn ? s.x[0] : 0;
        const u64 icount = s.icount;
        std::memcpy(&s, &saved, sizeof(State));
        s.icount = icount;
        if (ptr)
            found(name.c_str(), ptr);
    }
}

std::atomic<long long> g_learn_until_ms{0};

bool Native() {
    static const bool native = std::getenv("MUA3_NVN_NATIVE") != nullptr;
    return native;
}

void SetReplaySink(std::function<void(std::vector<ReplaySpan>&&)> sink) {
    g_sink = new std::function<void(std::vector<ReplaySpan>&&)>(std::move(sink)); // lives for the process
}

bool QueryTexture(u64 texture, TextureInfo& out) {
    auto& o = Obj();
    std::lock_guard lk(o.mutex);
    auto t = o.textures.find(texture);
    if (t == o.textures.end())
        return false;
    auto p = o.pools.find(t->second.pool);
    if (p == o.pools.end())
        return false;
    const TextureDesc& d = t->second;
    out = {TextureVa(p->second, d), d.width, d.height, d.depth, d.levels, d.format, d.target};
    return true;
}

bool QueryState(u64 object, std::string_view setter, u64 x[8], u32 v[4]) {
    auto& rec = Rec();
    std::lock_guard lk(rec.mutex);
    auto it = rec.shadow.find(object);
    if (it == rec.shadow.end())
        return false;
    for (auto c = it->second.rbegin(); c != it->second.rend(); ++c)
        if (g_generic[c->fn].name == setter) {
            std::memcpy(x, c->x, sizeof(c->x));
            std::memcpy(v, c->v, sizeof(c->v));
            return true;
        }
    return false;
}

u64 QueryProgramCode(u64 program) {
    auto& rec = Rec();
    std::lock_guard lk(rec.mutex);
    auto it = rec.shadow.find(program);
    if (it == rec.shadow.end())
        return 0;
    for (const Command& c : it->second)
        if (g_generic[c.fn].shaders && !c.extra.empty())
            return c.extra[0];
    return 0;
}

u64 QueryTexturePool(u64 pool) {
    auto& o = Obj();
    std::lock_guard lk(o.mutex);
    auto it = o.texture_pools.find(pool);
    return it == o.texture_pools.end() ? 0 : it->second;
}

recomp::Fn Find(std::string_view name, recomp::Fn driver, u64 pc) {
    static const bool enabled = [] {
        const char* e = std::getenv("MUA3_NVN_HLE");
        return !(e && e[0] == '0');
    }();
    {
        std::lock_guard lk(g_resolved_mutex);
        Resolved().emplace(std::string(name), pc);
    }
    if (!enabled)
        return nullptr;
    const Entry* entry = nullptr;
    for (const auto& e : kHostFunctions)
        if (e.name == name)
            entry = &e;
    if (driver && (!entry || driver != entry->fn)) {
        std::lock_guard lk(g_names_mutex);
        Driver().emplace(std::string(name), DriverEntry{driver, pc}); // the getters wrappers call
    }
    static const bool record = std::getenv("MUA3_NVN_RECORD") != nullptr || Native();
    if (!entry && record && driver) {
        Kind kind;
        if (!GenericKind(name, kind))
            return nullptr;
        for (int i = 0; i < g_generic_count; i++) // resolved again: reuse its slot (never wrap a wrapper)
            if (g_generic[i].name == name)
                return driver == kGenericFns[i] || g_generic[i].driver == driver ? kGenericFns[i] : nullptr;
        const int i = g_generic_count++;
        if (i >= kGenericSlots)
            return nullptr;
        GenericSlot& g = g_generic[i];
        g.name = std::string(name);
        g.driver = driver;
        g.kind = kind;
        g.draw = name.find("CommandBufferDraw") != std::string_view::npos;
        g.indirect = g.draw && name.find("Indirect") != std::string_view::npos;
        g.dispatch = name.find("DispatchCompute") != std::string_view::npos;
        g.defaults = name.find("SetDefaults") != std::string_view::npos;
        g.shaders = name == "nvnProgramSetShaders";
        g.rts = name == "nvnCommandBufferSetRenderTargets";
        if (name == "nvnCommandBufferCopyBufferToTexture") // (cb, src, dst, dstView, dstRegion, flags)
            g.copy_view[0] = 3, g.copy_region[0] = 4;
        else if (name == "nvnCommandBufferCopyTextureToBuffer") // (cb, src, srcView, srcRegion, dst, flags)
            g.copy_view[0] = 2, g.copy_region[0] = 3;
        else if (name == "nvnCommandBufferCopyTextureToTexture") // (cb, src, srcView, srcRegion, dst, dstView, dstRegion, flags)
            g.copy_view[0] = 2, g.copy_region[0] = 3, g.copy_view[1] = 5, g.copy_region[1] = 6;
        if (kind == Kind::Command && Native()) {
            // exactly what the host encoder implements; everything else (fences, counters, ...) still goes
            // to the driver, whose submissions run after the host's work of the same submit
            static const std::set<std::string_view> host = {
                "BindProgram", "BindUniformBuffer", "BindStorageBuffer", "BindTexture", "BindImage", "BindVertexBuffer",
                "BindVertexAttribState", "BindVertexStreamState", "SetRenderTargets", "SetViewports", "SetDepthRanges",
                "SetScissors", "SetScissor", "BindBlendState", "BindColorState", "BindChannelMaskState",
                "BindDepthStencilState", "BindPolygonState", "BindMultisampleState", "Barrier", "SetDepthBounds",
                "SetPatchSize", "SetShaderScratchMemory", "SetRenderEnable", "SetRenderEnableConditional",
                "SetStencilRef", "SetStencilValueMask", "SetStencilMask", "SetPolygonOffsetClamp", "SetPrimitiveRestart",
                "SetTexturePool", "SetSamplerPool", "ClearColor", "ClearDepthStencil", "DrawArrays", "DrawElements",
                "DrawElementsBaseVertex", "DrawElementsIndirect", "DrawArraysIndirect", "DispatchCompute",
                "CopyBufferToBuffer", "CopyBufferToTexture", "CopyTextureToBuffer", "SetCopyRowStride",
                "SetCopyImageStride"};
            g.host = host.count(name.substr(16)) != 0;
        }
        for (std::string_view k : {"SetBlendEnable", "SetChannelMask", "SetStencilFunc", "SetStencilOp", "SetStencilMask",
                                   "SetStencilValueMask", "SetStencilRef"})
            g.indexed |= kind == Kind::Setter && name.find(k) != std::string_view::npos;
        auto raw = [&](int ptr, int count, u32 elem, bool objects) {
            g.raw_ptr = ptr, g.raw_count = count, g.raw_elem = elem, g.objects = objects;
        };
        if (name == "nvnCommandBufferSetViewports" || name == "nvnCommandBufferSetScissors")
            raw(3, 2, 16, false); // (cb, first, count, float/int[4] array)
        else if (name == "nvnCommandBufferSetDepthRanges")
            raw(3, 2, 8, false);
        else if (name == "nvnCommandBufferBindVertexAttribState")
            raw(2, 1, 4, false); // (cb, count, NVNvertexAttribState array): decoded from the raw words
        else if (name == "nvnCommandBufferBindVertexStreamState")
            raw(2, 1, 8, false);
        else if (name == "nvnCommandBufferClearColor")
            raw(2, -1, 16, false); // (cb, index, const float rgba[4], mask)
        else if (name == "nvnCommandBufferBindProgram")
            raw(1, -1, 8, true);
        else if (name.substr(0, 20) == "nvnCommandBufferBind" && name.ends_with("State"))
            raw(1, -1, 32, true); // Bind{Blend,Color,ChannelMask,DepthStencil,Polygon,Multisample}State(cb, state)
        return kGenericFns[i];
    }
    if (!entry || (entry->record && !record))
        return nullptr;
    if (entry->wrapped >= 0) {
        if (driver == entry->fn) // resolved again after the swap
            return entry->fn;
        if (!driver) // wrappers need the driver's code to run
            return nullptr;
        if (!g_driver[entry->wrapped])
            g_driver[entry->wrapped] = driver;
    }
    return entry->fn;
}

} // namespace hle::nvn
