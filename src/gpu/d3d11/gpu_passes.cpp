#include "gpu/d3d11/gpu_passes.h"

#include <algorithm>
#include <cstdlib>


namespace gpu {

void GpuPasses::Init(ID3D11Device* dev, ID3D11DeviceContext* ctx) {
    dev_ = dev;
    ctx_ = ctx;
    const char* e = std::getenv("MUA3_GPU_PASSES");
    env_on_ = on_ = e && *e && *e != '0';
    per_program_ = e && *e == '2';
    if (!on_)
        return;
    on_ = CreateQueries();
    LOG_INFO("Renderer", "GPU pass timing on (MUA3_GPU_PASSES)");
}

bool GpuPasses::CreateQueries() {
    if (created_)
        return true;
    for (auto& s : slots_) {
        D3D11_QUERY_DESC qd{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
        if (FAILED(dev_->CreateQuery(&qd, &s.disjoint)))
            return false;
    }
    return created_ = true;
}

void GpuPasses::StartReport(u32 frames) {
    if (!dev_ || !frames || !CreateQueries())
        return;
    for (auto& s : slots_) // (results of earlier frames are not part of the report)
        if (s.pending)
            Collect(s);
    agg_.clear();
    collected_ = 0;
    total_ms_ = 0;
    report_frames_ = frames;
    report_.clear();
    on_ = true;
}

void GpuPasses::Stamp(u64 key) {
    Slot& s = slots_[cur_];
    if (!open_) {
        if (s.pending)
            Collect(s);
        ctx_->Begin(s.disjoint.Get());
        s.used = 0;
        s.keys.clear();
        s.draws.clear();
        open_ = true;
    }
    if (s.used == s.ts.size()) {
        D3D11_QUERY_DESC qd{D3D11_QUERY_TIMESTAMP, 0};
        Microsoft::WRL::ComPtr<ID3D11Query> q;
        if (FAILED(dev_->CreateQuery(&qd, &q)))
            return;
        s.ts.push_back(q);
    }
    ctx_->End(s.ts[s.used].Get());
    if (!s.keys.empty())
        s.draws.back() = draws_;
    s.keys.push_back(key);
    s.draws.push_back(0);
    s.used++;
    draws_ = 0;
}

void GpuPasses::Mark(u64 key, const std::function<std::string()>& describe) {
    if (!on_ || key == key_)
        return;
    key_ = key;
    if (!names_.count(key))
        names_[key] = describe();
    Stamp(key);
}

void GpuPasses::EndFrame() {
    if (!on_ || !open_)
        return;
    Stamp(~0ull); // (closes the last pass)
    Slot& s = slots_[cur_];
    ctx_->End(s.disjoint.Get());
    s.pending = true;
    open_ = false;
    key_ = ~0ull;
    cur_ = (cur_ + 1) % kSlots;
    frames_++;
    if (report_frames_ && collected_ >= report_frames_) {
        std::vector<std::pair<u64, Agg>> v(agg_.begin(), agg_.end());
        std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second.ms > b.second.ms; });
        const double n = double(collected_);
        report_ = fmt::format("GPU {:.3f} ms per frame over {} frames, {} kinds of pass (a pass also holds the copies and "
                              "uploads issued inside it)\n   ms/frame  passes  draws  pass\n",
                              total_ms_ / n, collected_, v.size());
        for (size_t i = 0; i < v.size() && i < 60; i++)
            report_ += fmt::format("  {:9.3f}  {:6.1f}  {:5.0f}  {}\n", v[i].second.ms / n, v[i].second.count / n,
                                   v[i].second.draws / n, names_[v[i].first]);
        report_frames_ = 0;
        agg_.clear();
        collected_ = 0;
        total_ms_ = 0;
        on_ = env_on_;
        return;
    }
    if (!env_on_ || frames_ % 300 != 0 || !collected_)
        return;
    std::vector<std::pair<u64, Agg>> v(agg_.begin(), agg_.end());
    std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second.ms > b.second.ms; });
    const double n = double(collected_);
    LOG_INFO("GpuPasses", "GPU {:.3f} ms/frame over {} frames in {} kinds of pass (top 30):", total_ms_ / n, collected_, v.size());
    for (size_t i = 0; i < v.size() && i < 30; i++)
        LOG_INFO("GpuPasses", "  {:7.3f} ms  {:5.1f}x  {:6.1f} draws  {}", v[i].second.ms / n, v[i].second.count / n,
                 v[i].second.draws / n, names_[v[i].first]);
    agg_.clear();
    collected_ = 0;
    total_ms_ = 0;
}

void GpuPasses::Collect(Slot& s) {
    s.pending = false;
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
    while (ctx_->GetData(s.disjoint.Get(), &dj, sizeof(dj), 0) == S_FALSE) {
    }
    std::vector<u64> t(s.used, 0);
    for (u32 i = 0; i < s.used; i++)
        while (ctx_->GetData(s.ts[i].Get(), &t[i], sizeof(u64), 0) == S_FALSE) {
        }
    if (dj.Disjoint || !dj.Frequency || s.used < 2)
        return;
    const double to_ms = 1000.0 / double(dj.Frequency);
    for (u32 i = 0; i + 1 < s.used; i++) {
        const double ms = double(t[i + 1] - t[i]) * to_ms;
        Agg& a = agg_[s.keys[i]];
        a.ms += ms;
        a.draws += s.draws[i];
        a.count++;
        total_ms_ += ms;
    }
    collected_++;
}

} // namespace gpu
