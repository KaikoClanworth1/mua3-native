#include "core/hle/nvn/nvn_hle.h"

#include <array>
#include <chrono>
#include <bit>
#include <set>
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
};
struct BufferDesc {
    u64 pool = 0, offset = 0, size = 0, gpu = 0;
};
struct TextureDesc {
    u64 pool = 0, offset = 0, storage = 0;
    u32 width = 0, height = 0, depth = 0, levels = 0, format = 0, target = 0;
};
struct Objects {
    std::mutex mutex;
    std::unordered_map<u64, PoolDesc> pools;
    std::unordered_map<u64, BufferDesc> buffers;
    std::unordered_map<u64, TextureDesc> textures;
    // texture/sampler pool slots: the IDs packed into texture handles (nvnDeviceGetTextureHandle)
    std::unordered_map<u32, std::pair<u64, u64>> texture_ids; // id -> (texture, texture view or 0)
    std::unordered_map<u32, u64> sampler_ids;                 // id -> sampler
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
void TextureInitialize(State& s) {
    u64 tex, ok;
    if (!Through(s, kTextureInitialize, tex, ok) || !(ok & 1))
        return;
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
struct Command {
    int fn;                   // generic slot (name), or -1 for Call/CopyCommands
    u64 x[8];                 // integer/pointer arguments (x0 = the object)
    u32 v[4];                 // float arguments (low 32 bits of v0..v3)
    std::vector<u64> handles; // Call/CopyCommands: nested command handles; ProgramSetShaders: stage data pairs
};
enum class Kind { Command, Setter };
struct GenericSlot {
    std::string name;
    recomp::Fn driver = nullptr;
    Kind kind = Kind::Command;
    bool draw = false, indirect = false, dispatch = false, defaults = false, shaders = false;
};
constexpr int kGenericSlots = 320;
std::array<GenericSlot, kGenericSlots> g_generic;
std::atomic<int> g_generic_count{0};
struct Recording {
    std::mutex mutex;
    std::unordered_map<u64, std::vector<Command>> open;    // command buffer -> commands since BeginRecording
    std::unordered_map<u64, std::vector<Command>> handles; // command handle -> finished list
    u64 submits = 0, cmds = 0, draws = 0, indirect = 0, dispatches = 0, unknown = 0;
    u64 recorded = 0, begins = 0, ends = 0, empty_ends = 0; // diagnostics
    // state objects (blend, colour, depth/stencil, polygon, multisample, channel mask, vertex attrib/stream,
    // texture views, sampler builders, programs): object -> generic slot -> last call
    std::unordered_map<u64, std::unordered_map<int, Command>> shadow;
};
Recording& Rec() {
    static Recording r;
    return r;
}
template <int I>
void Generic(State& s) {
    const GenericSlot& g = g_generic[I];
    Command c{I, {}, {}, {}};
    for (int k = 0; k < 8; k++)
        c.x[k] = s.x[k];
    for (int k = 0; k < 4; k++)
        c.v[k] = u32(s.v[k].lo);
    if (g.shaders) // nvnProgramSetShaders(program, count, const NVNshaderData*): {code address, control pointer}
        for (u64 i = 0; i < u32(s.x[1]) && i < 8; i++) {
            c.handles.push_back(recomp::Rd<u64>(s, s.x[2] + i * 16));
            c.handles.push_back(recomp::Rd<u64>(s, s.x[2] + i * 16 + 8));
        }
    {
        auto& rec = Rec();
        std::lock_guard lk(rec.mutex);
        if (g.kind == Kind::Command) {
            rec.open[s.x[0]].push_back(std::move(c));
            rec.recorded++;
        } else {
            auto& obj = rec.shadow[s.x[0]];
            if (g.defaults)
                obj.clear();
            obj[I] = std::move(c);
        }
    }
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
    Command c{-1, {cb, count}, {}, {}};
    for (u64 i = 0; i < count && i < 4096; i++)
        c.handles.push_back(recomp::Rd<u64>(s, array + i * 8));
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
    rec.open[cb].clear();
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
    auto& slot = rec.handles[handle];
    if (!rec.open[cb].empty() || slot.empty())
        slot = std::move(rec.open[cb]);
    rec.open[cb].clear();
}
// void nvnQueueSubmitCommands(NVNqueue*, int numCommands, const NVNcommandHandle* handles)
void QueueSubmitCommands(State& s) {
    const u64 count = u32(s.x[1]), array = s.x[2];
    std::vector<u64> handles;
    for (u64 i = 0; i < count && i < 256; i++)
        handles.push_back(recomp::Rd<u64>(s, array + i * 8));
    // Observed before the driver runs: the submit makes a GPU syscall that can hand the rest of the call to the JIT.
    const u64 queue = s.x[0];
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
        if (auto it = rec.handles.find(h); it != rec.handles.end())
            top += it->second.size();
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
            f << fmt::format("handle {:#x} ({} commands)\n", h, it == rec.handles.end() ? 0 : it->second.size());
            if (it == rec.handles.end())
                continue;
            for (const Command& c : it->second) {
                const std::string name = c.fn >= 0 ? g_generic[c.fn].name : "CallCommands";
                f << fmt::format("  {} {:x} {:x} {:x} {:x} {:x} {:x} {:x} | {} {} {} {}\n", name, c.x[1], c.x[2], c.x[3],
                                 c.x[4], c.x[5], c.x[6], c.x[7], std::bit_cast<float>(c.v[0]),
                                 std::bit_cast<float>(c.v[1]), std::bit_cast<float>(c.v[2]),
                                 std::bit_cast<float>(c.v[3]));
                if (name.find("State") != std::string::npos || name.find("BindProgram") != std::string::npos)
                    objects.insert(c.x[1]);
                if (depth < 8)
                    for (u64 nested : c.handles)
                        walk.emplace_back(nested, depth + 1);
            }
        }
        for (u64 obj : objects) {
            auto it = rec.shadow.find(obj);
            if (it == rec.shadow.end())
                continue;
            f << fmt::format("object {:#x}\n", obj);
            for (const auto& [slot, c] : it->second) {
                f << fmt::format("  {} {:x} {:x} {:x} {:x} {:x} | {} {}", g_generic[slot].name, c.x[1], c.x[2], c.x[3],
                                 c.x[4], c.x[5], std::bit_cast<float>(c.v[0]), std::bit_cast<float>(c.v[1]));
                for (u64 h : c.handles)
                    f << fmt::format(" {:x}", h);
                f << "\n";
            }
        }
        LOG_INFO("NVN", "wrote nvn_frame.txt");
    }
    // walk the submitted command buffers, following nested Call/CopyCommands
    std::vector<std::pair<u64, int>> work;
    for (u64 h : handles)
        work.emplace_back(h, 0);
    while (!work.empty()) {
        const auto [h, depth] = work.back();
        work.pop_back();
        auto it = rec.handles.find(h);
        if (it == rec.handles.end()) {
            rec.unknown++;
            continue;
        }
        for (const Command& c : it->second) {
            rec.cmds++;
            if (c.fn >= 0) {
                const GenericSlot& g = g_generic[c.fn];
                rec.draws += g.draw;
                rec.indirect += g.indirect;
                rec.dispatches += g.dispatch;
            }
            if (depth < 8)
                for (u64 nested : c.handles)
                    work.emplace_back(nested, depth + 1);
        }
    }
    if (++rec.submits % 600 == 0) {
        LOG_INFO("NVN", "recorded stream, per submit: {:.0f} commands, {:.0f} draws ({:.0f} indirect), {:.1f} "
                        "dispatches; {} unrecorded handles (recorded {}, begins {}, ends {}, empty {})",
                 double(rec.cmds) / 600, double(rec.draws) / 600, double(rec.indirect) / 600,
                 double(rec.dispatches) / 600, rec.unknown, rec.recorded, rec.begins, rec.ends, rec.empty_ends);
        rec.cmds = rec.draws = rec.indirect = rec.dispatches = rec.unknown = 0;
    }
    }
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
    static const bool record = std::getenv("MUA3_NVN_RECORD") != nullptr;
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
