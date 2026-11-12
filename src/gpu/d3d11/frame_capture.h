// One-frame graphics capture for the F4 bug capture (see platform/bug_capture.cpp and docs/bug_capture.md).
// Armed from any thread; the renderer feeds it on the GPU thread (under the context lock) from the next presented
// frame to the one after, then writes everything at that second present. Costs nothing while not armed (the
// renderer checks one pointer per draw).
#pragma once
#include "common/common.h"
#undef DrawState
#include "gpu/draw_state.h"

#include <atomic>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <d3d11.h>
#include <wrl/client.h>
#undef DrawState // (a Win32 macro: windows.h renames it DrawStateW)

namespace gpu {

class D3D11Renderer;
struct Surface;
struct TicEntry;

// What the DDS writer needs of a captured texture (2D, 2D array, cube or 3D).
struct TexDesc {
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    u32 width = 0, height = 0, depth = 1, mips = 1, array = 1;
    bool cube = false, volume = false;
};

class FrameCapture {
public:
    // dir: the output folder (frame/ is created inside); max_bytes: budget for textures, buffers and target images
    // (shares: textures 2/5, guest and compute buffers 2/5, target images 1/5).
    FrameCapture(fs::path dir, u64 max_bytes);
    ~FrameCapture();

    bool Recording() const { return state_ == kRecording; }
    bool Done() const { return state_ == kDone; }
    const std::string& Summary() const { return summary_; }

    // ---- GPU thread, context lock held
    void BeginDraw(const DrawState& m);
    void Texture(bool vs, u32 slot, u32 handle, const TicEntry& tic, ID3D11ShaderResourceView* srv, bool render_target);
    void Submit(u64 vs_hash, u64 ps_hash, u32 index_count);
    void Dispatch(const ComputeLaunch& l);
    // The dispatch just recorded: its host compute shader (hash; its guest program is copied from shader_dump) and
    // each global memory window's contents on the host GPU before and after it ran.
    void ComputeShader(u64 cs_hash);
    void Gmem(u32 window, u64 gpu_va, const std::vector<u8>& before, const std::vector<u8>& after);
    void Clear(const std::string& what);
    // Each present: starts recording at the first, writes the capture at the second (then returns true and the
    // renderer drops it).
    bool Present(D3D11Renderer& r, Surface* presented, u64 cpu_addr);

private:
    enum State { kArmed, kRecording, kDone };
    std::string SaveBuffer(u64 gpu_va, u64 size, const char* kind); // relative path, "" when not saved
    std::string SaveData(const std::vector<u8>& data, const char* kind); // by content hash, within the budget
    void FlushDraw();
    void Finish(D3D11Renderer& r, Surface* presented);

    struct PendingTex {
        Microsoft::WRL::ComPtr<ID3D11Resource> staging; // Texture2D or Texture3D
        TexDesc desc;
        std::string path;
    };

    fs::path dir_;
    enum { kTex, kBuf, kRt }; // budget shares: textures 2/5, buffers 2/5, target images 1/5
    u64 budget_[3], used_[3]{};
    std::atomic<int> state_{kArmed};
    std::string summary_;
    u64 index_ = 0, draws_ = 0, drawn_ = 0, dispatches_ = 0, clears_ = 0, skipped_bytes_ = 0;
    std::vector<u8> states_;           // DrawState of every draw, back to back (states.bin)
    std::string events_;               // events.jsonl
    std::string cur_;                  // the open draw's JSON (written at the next draw / present)
    std::string cur_tex_;
    bool cur_open_ = false;
    bool cur_dispatch_ = false;        // the open event is a dispatch (closed with its cs / gmem)
    std::map<u64, std::string> saved_;            // content hash -> file
    std::map<u64, std::string> ranges_;           // vertex stream address/size -> file
    std::map<ID3D11Resource*, std::string> texs_; // texture resource -> its .dds
    std::vector<PendingTex> pending_;
    std::set<u64> shaders_;                       // vs/ps/cs hashes used
    std::vector<u64> targets_;                    // colour targets written, first-write order
};

} // namespace gpu
