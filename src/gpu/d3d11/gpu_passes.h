#pragma once
// MUA3_GPU_PASSES=1: GPU time per render pass (a run of work into the same render targets, or a run of compute
// dispatches), from timestamp queries read a few frames later. Every 300 frames the log lists the passes by GPU
// ms per frame. Off: no queries, no cost.
#include <functional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <d3d11.h>
#include <wrl/client.h>

#include "common/common.h"

namespace gpu {

class GpuPasses {
public:
    bool On() const { return on_; }
    void Init(ID3D11Device* dev, ID3D11DeviceContext* ctx);
    // Device lost: drops the queries (Init again on the new device); a running report is abandoned.
    void ReleaseDevice() {
        for (auto& s : slots_)
            s = Slot{};
        created_ = open_ = false;
        on_ = false;
        report_frames_ = 0;
        key_ = ~0ull;
        dev_ = nullptr, ctx_ = nullptr;
    }
    // A new pass starts here (no-op if `key` is the current pass). `describe` is called once per new key.
    void Mark(u64 key, const std::function<std::string()>& describe);
    void CountDraw() { draws_++; }
    // Called once per presented frame, after its last GPU work.
    void EndFrame();
    // One-shot report (the F4 capture): times the next `frames` frames, compute per program, even without
    // MUA3_GPU_PASSES; TakeReport then returns the table (empty until it is ready).
    void StartReport(u32 frames);
    std::string TakeReport() { return std::exchange(report_, {}); }
    // Compute dispatches are timed per program (MUA3_GPU_PASSES=2, or a report is running).
    bool PerProgram() const { return per_program_ || report_frames_; }

private:
    struct Slot {
        Microsoft::WRL::ComPtr<ID3D11Query> disjoint;
        std::vector<Microsoft::WRL::ComPtr<ID3D11Query>> ts;
        std::vector<u64> keys;
        std::vector<u32> draws;
        u32 used = 0;
        bool pending = false;
    };
    struct Agg {
        double ms = 0;
        u64 draws = 0;
        u32 count = 0; // passes with this key
    };
    void Stamp(u64 key);
    void Collect(Slot& s);
    bool CreateQueries();
    bool on_ = false;
    bool env_on_ = false, per_program_ = false, created_ = false;
    u32 report_frames_ = 0; // frames the running report still needs (0: none running)
    std::string report_;
    ID3D11Device* dev_ = nullptr;
    ID3D11DeviceContext* ctx_ = nullptr;
    static constexpr u32 kSlots = 5;
    Slot slots_[kSlots];
    u32 cur_ = 0;
    bool open_ = false;
    u64 key_ = ~0ull;
    u32 draws_ = 0;
    u64 frames_ = 0, collected_ = 0;
    double total_ms_ = 0;
    std::unordered_map<u64, Agg> agg_;
    std::unordered_map<u64, std::string> names_;
};

} // namespace gpu
