#pragma once
// Host implementations of NVN API functions. The game resolves every NVN entry point by name through
// nvnDeviceGetProcAddress and calls it through a pointer; the CPU runtime records which driver function each name
// resolves to and, when a host implementation exists, runs that instead of the sdk's NVN driver.
// See docs/native_renderer_plan.md.
#include <algorithm>
#include <atomic>
#include <functional>
#include <memory>
#include <string_view>
#include <iterator>
#include <vector>

#include "core/cpu/recomp/recomp_rt.h"

namespace hle::nvn {

// Host implementation for an NVN entry point name, or nullptr if the guest driver still handles it. `driver` is the
// driver's own (recompiled) implementation at guest address `pc`, which observing wrappers call through to. Host functions follow the
// guest calling convention: arguments in s.x / s.v, result in s.x[0], and they return with s.pc = s.x[30] like a
// recompiled function.
recomp::Fn Find(std::string_view name, recomp::Fn driver, recomp::u64 pc);

// Asks the driver's name lookup function (guest `lookup_pc`) for every host-implemented entry point not seen yet
// (some are resolved by code the runtime does not observe) and reports each through `found`.
void ResolveMissing(recomp::State& s, recomp::Fn lookup, recomp::u64 lookup_pc, recomp::u64 device,
                    const std::function<void(const char*, recomp::u64)>& found);

// MUA3_NVN_LEARN: end of the current learning window (steady_clock ms), opened when MUA3_NVN_DUMP writes a frame,
// so the emulated path's resolutions can be joined with that same frame's NVN commands.
extern std::atomic<long long> g_learn_until_ms;

// MUA3_NVN_TIMELINE=<seconds>: after that long, records 3,000 events (submit, fence waits, acquire, present, GPU jobs)
// with thread ids and times to nvn_timeline.txt.
void TimelineEvent(const char* what, recomp::u64 arg = 0);

// ---- native rendering: command buffer commands the host encoder handles are recorded and never reach the NVN
// driver. Each submitted frame's commands, flattened in execution order (nested Call/CopyCommands expanded), go to
// the sink (gpu/nvn_encoder.cpp); queries resolve the objects they reference.
struct StateCall {
    std::string_view name; // setter, e.g. "nvnBlendStateSetBlendFunc"
    recomp::u64 x[8];      // x[0] = the object
    recomp::u32 v[4];
    std::vector<recomp::u64> handles; // nvnProgramSetShaders: {code address, control pointer} per stage
};
// A few words kept inline (thousands of commands a frame carry a handful each): no heap traffic unless more. Kept
// small on purpose: commands are written by the game's NVN thread (the frame's critical path) and a smaller command
// is less memory to write (ReplayCommand was ~320 bytes, ~190 now).
template <size_t N>
class SmallWords {
public:
    SmallWords() = default;
    SmallWords(SmallWords&& o) noexcept : n_(o.n_), heap_(std::move(o.heap_)) {
        if (n_ <= N)
            std::copy(o.inl_, o.inl_ + n_, inl_);
        o.n_ = 0;
    }
    SmallWords& operator=(SmallWords&& o) noexcept {
        n_ = o.n_;
        heap_ = std::move(o.heap_);
        if (n_ <= N)
            std::copy(o.inl_, o.inl_ + n_, inl_);
        o.n_ = 0;
        return *this;
    }
    SmallWords(const SmallWords& o) : n_(0) { assign(o.begin(), o.end()); }
    SmallWords& operator=(const SmallWords& o) {
        if (this != &o)
            assign(o.begin(), o.end());
        return *this;
    }
    size_t size() const { return n_; }
    bool empty() const { return n_ == 0; }
    recomp::u64* data() { return n_ <= N ? inl_ : heap_->data(); }
    const recomp::u64* data() const { return n_ <= N ? inl_ : heap_->data(); }
    recomp::u64& operator[](size_t i) { return data()[i]; }
    const recomp::u64& operator[](size_t i) const { return data()[i]; }
    recomp::u64* begin() { return data(); }
    recomp::u64* end() { return data() + n_; }
    const recomp::u64* begin() const { return data(); }
    const recomp::u64* end() const { return data() + n_; }
    std::reverse_iterator<const recomp::u64*> rbegin() const { return std::reverse_iterator<const recomp::u64*>(end()); }
    std::reverse_iterator<const recomp::u64*> rend() const { return std::reverse_iterator<const recomp::u64*>(begin()); }
    void push_back(recomp::u64 v) {
        if (n_ < N)
            inl_[n_] = v;
        else {
            if (n_ == N) {
                if (!heap_)
                    heap_ = std::make_unique<std::vector<recomp::u64>>();
                heap_->assign(inl_, inl_ + N);
            }
            heap_->push_back(v);
        }
        n_++;
    }
    void clear() {
        n_ = 0;
        if (heap_)
            heap_->clear();
    }
    template <typename It>
    void assign(It a, It b) {
        std::vector<recomp::u64> tmp(a, b); // (the source may be this object's own storage)
        clear();
        for (recomp::u64 v : tmp)
            push_back(v);
    }

private:
    recomp::u32 n_ = 0;
    recomp::u64 inl_[N];
    std::unique_ptr<std::vector<recomp::u64>> heap_; // everything, once there are more than N
};
// A vector that costs one pointer until something is put in it (most commands have no state objects).
template <typename T>
class LazyVector {
public:
    LazyVector() = default;
    LazyVector(LazyVector&&) noexcept = default;
    LazyVector& operator=(LazyVector&&) noexcept = default;
    LazyVector(const LazyVector& o) : v_(o.v_ ? std::make_unique<std::vector<T>>(*o.v_) : nullptr) {}
    LazyVector& operator=(const LazyVector& o) {
        v_ = o.v_ ? std::make_unique<std::vector<T>>(*o.v_) : nullptr;
        return *this;
    }
    bool empty() const { return !v_ || v_->empty(); }
    size_t size() const { return v_ ? v_->size() : 0; }
    T& operator[](size_t i) { return (*v_)[i]; }
    const T& operator[](size_t i) const { return (*v_)[i]; }
    T* begin() { return v_ ? v_->data() : nullptr; }
    T* end() { return v_ ? v_->data() + v_->size() : nullptr; }
    const T* begin() const { return v_ ? v_->data() : nullptr; }
    const T* end() const { return v_ ? v_->data() + v_->size() : nullptr; }
    T& emplace_back() { return Get().emplace_back(); }
    void reserve(size_t n) { Get().reserve(n); }

private:
    std::vector<T>& Get() {
        if (!v_)
            v_ = std::make_unique<std::vector<T>>();
        return *v_;
    }
    std::unique_ptr<std::vector<T>> v_;
};
struct ReplayCommand {
    std::string_view name;          // NVN entry point, e.g. "nvnCommandBufferDrawElementsBaseVertex" (empty: nested)
    recomp::u64 x[8]{};             // integer/pointer arguments (x[0] = command buffer)
    recomp::u32 v[4]{};             // float arguments
    int fn = -1; // recorder slot (-1: Call/CopyCommands, whose nested handles are in `extra`)
    SmallWords<2> extra;            // captured arrays (SetRenderTargets colour textures, ...)
    SmallWords<4> raw;              // memory the command points at, read at record time (viewports, state objects)
    // state objects the command binds (BindProgram, Bind*State, vertex attrib/stream arrays): their setter calls
    // since SetDefaults, as of record time (the game builds many of them on the stack)
    LazyVector<std::vector<StateCall>> objects;
};
// A submit, in execution order: ranges of recorded command lists (shared, never copied).
struct ReplaySpan {
    std::shared_ptr<const std::vector<ReplayCommand>> list;
    size_t begin = 0, end = 0;
};
void SetReplaySink(std::function<void(std::vector<ReplaySpan>&&)> sink);

struct TextureInfo {
    recomp::u64 gpu = 0; // texture GPU address (texture mapping)
    recomp::u32 width = 0, height = 0, depth = 0, levels = 0, format = 0, target = 0;
};
bool QueryTexture(recomp::u64 texture, TextureInfo& out);
// Last recorded call of `setter` (e.g. "nvnBlendStateSetBlendFunc") on a state object; false if never set.
bool QueryState(recomp::u64 object, std::string_view setter, recomp::u64 x[8], recomp::u32 v[4]);
// Shader code GPU address of a single-stage program (the SPH is at +0x30), or 0.
recomp::u64 QueryProgramCode(recomp::u64 program);
// Texture / sampler pool descriptor base GPU addresses (TIC / TSC tables), or 0.
recomp::u64 QueryTexturePool(recomp::u64 pool);

} // namespace hle::nvn
