// GPU time of each event (draw, dispatch, clear) of the F4 frame capture: a timestamp query pair right around the
// event's own D3D11 call, so CPU work between events (the capture writing its files, the renderer preparing the
// next draw) is not counted. Results are read once, when the capture is written.
#pragma once
#include "common/common.h"

#include <map>
#include <vector>

#include <d3d11.h>
#include <wrl/client.h>

namespace gpu {

class CaptureTiming {
public:
    // Brackets event `event`'s D3D11 call (GPU thread, context lock held). Begin starts the frame's timing on its
    // first call.
    void Begin(ID3D11DeviceContext* ctx, u64 event);
    void End(ID3D11DeviceContext* ctx, u64 event);

    struct Result {
        std::map<u64, double> us; // event -> GPU microseconds (summed when an event has several calls)
        double frame_ms = 0;      // first Begin to last End
        double events_ms = 0;     // sum of all events
        bool ok = false;          // false: no timestamps, or the GPU clock was disjoint
    };
    // Waits for the queries (up to ~2 s) and returns the times.
    Result Finish();

private:
    struct Pair {
        u64 event;
        Microsoft::WRL::ComPtr<ID3D11Query> begin, end;
    };
    ID3D11DeviceContext* ctx_ = nullptr;
    Microsoft::WRL::ComPtr<ID3D11Device> dev_;
    Microsoft::WRL::ComPtr<ID3D11Query> disjoint_;
    std::vector<Pair> pairs_;
    bool open_ = false; // a Begin waiting for its End
};

} // namespace gpu
