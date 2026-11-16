#pragma once
// NVIDIA DLSS Super Resolution (mua3.ini [video] upscaler=2), run by mua3_dlss.dll: our NGX code with NVIDIA's library
// linked in (src/dlss), loaded at run time from next to mua3.exe. The game never links NVIDIA's SDK; without the DLL
// (any build made without the SDK in third_party/dlss) DLSS reports itself unavailable and FSR 1 upscales instead.
// DLSS is a trademark of NVIDIA Corporation.
#include <string>
#include <string_view>
#include <d3d11.h>

#include "common/common.h"

namespace gpu {

class Dlss {
public:
    ~Dlss();
    // Loads mua3_dlss.dll and starts NGX on `dev` (once): can DLSS Super Resolution run here?
    bool Available(ID3D11Device* dev);
    // Cheap guess without loading anything (for the menus): mua3_dlss.dll is installed and `dev` is an NVIDIA GPU.
    static bool Likely(ID3D11Device* dev);
    // Why it is not available (or "ready"), for the log and the menu.
    const std::string& Status() const { return status_; }

    struct Frame {
        ID3D11Resource* color = nullptr;  // the scene, in_w x in_h (jittered)
        ID3D11Resource* depth = nullptr;  // its depth, the same size
        ID3D11Resource* motion = nullptr; // RG16F, pixels from the current to the previous position, the same size
        ID3D11Resource* output = nullptr; // out_w x out_h, UAV-capable
        u32 in_w = 0, in_h = 0, out_w = 0, out_h = 0;
        float jitter_x = 0, jitter_y = 0; // the sub-pixel offset the scene was rendered at, in input pixels
        bool reset = false;               // no usable history (a cut, a new level)
        int quality = 1;                  // upscale_quality (0 Ultra Quality .. 3 Performance)
    };
    // Upscales one frame. Leaves the context's state changed.
    bool Evaluate(ID3D11Device* dev, ID3D11DeviceContext* ctx, const Frame& f);
    // Releases DLSS and NGX (shutdown); Reset also lets the next Available() try again (a new device).
    void Shutdown();
    void Reset();

private:
    bool tried_ = false, ok_ = false;
    std::string status_ = "not initialised";
    ID3D11Device* dev_ = nullptr;
};

} // namespace gpu
