#include "gpu/d3d11/capture_timing.h"

#include <chrono>
#include <thread>

namespace gpu {

void CaptureTiming::Begin(ID3D11DeviceContext* ctx, u64 event) {
    if (!ctx_) {
        ctx_ = ctx;
        ctx->GetDevice(&dev_);
        D3D11_QUERY_DESC qd{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
        if (FAILED(dev_->CreateQuery(&qd, &disjoint_)))
            return;
        ctx->Begin(disjoint_.Get());
    }
    if (!disjoint_ || ctx != ctx_)
        return;
    Pair p{event};
    D3D11_QUERY_DESC qd{D3D11_QUERY_TIMESTAMP, 0};
    if (FAILED(dev_->CreateQuery(&qd, &p.begin)) || FAILED(dev_->CreateQuery(&qd, &p.end)))
        return;
    ctx->End(p.begin.Get());
    pairs_.push_back(std::move(p));
    open_ = true;
}

void CaptureTiming::End(ID3D11DeviceContext* ctx, u64 event) {
    if (!open_ || ctx != ctx_ || pairs_.empty() || pairs_.back().event != event)
        return;
    ctx->End(pairs_.back().end.Get());
    // Submit the pair now: otherwise a driver flush can fall between the two stamps and the rest wait in the
    // driver while the capture writes files (draws then read tens of milliseconds).
    ctx->Flush();
    open_ = false;
}

CaptureTiming::Result CaptureTiming::Finish() {
    Result r;
    if (!disjoint_)
        return r;
    if (open_) // (a Begin without its End: drop it)
        pairs_.pop_back(), open_ = false;
    ctx_->End(disjoint_.Get());
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    HRESULT hr;
    while ((hr = ctx_->GetData(disjoint_.Get(), &dj, sizeof(dj), 0)) == S_FALSE && std::chrono::steady_clock::now() < until)
        std::this_thread::yield();
    if (hr != S_OK || dj.Disjoint || !dj.Frequency)
        return r;
    const double to_us = 1e6 / double(dj.Frequency);
    u64 lo = ~0ull, hi = 0;
    for (auto& p : pairs_) {
        u64 b = 0, e = 0;
        if (ctx_->GetData(p.begin.Get(), &b, sizeof(b), 0) != S_OK || ctx_->GetData(p.end.Get(), &e, sizeof(e), 0) != S_OK ||
            e < b)
            continue;
        const double us = double(e - b) * to_us;
        r.us[p.event] += us;
        r.events_ms += us / 1000;
        lo = std::min(lo, b);
        hi = std::max(hi, e);
    }
    if (hi > lo)
        r.frame_ms = double(hi - lo) * to_us / 1000;
    r.ok = !r.us.empty();
    return r;
}

} // namespace gpu
