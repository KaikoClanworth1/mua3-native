#pragma once
// The C interface of mua3_dlss.dll: our DLSS Super Resolution code with NVIDIA's NGX library linked in. The game loads
// it at run time when it is next to mua3.exe (with NVIDIA's nvngx_dlss.dll); the game itself never links NGX.
// mua3_dlss.dll is built only where NVIDIA's SDK is in third_party/dlss (CMake MUA3_DLSS), never from the public repo.
#include <d3d11.h>

#define MUA3_DLSS_API_VERSION 1

struct Mua3DlssFrame {
    ID3D11Resource* color;  // the scene, in_w x in_h, rendered with the jitter below
    ID3D11Resource* depth;  // its depth, the same size
    ID3D11Resource* motion; // RG16F, pixels from the current to the previous position, the same size, not jittered
    ID3D11Resource* output; // out_w x out_h, UAV-capable
    unsigned in_w, in_h, out_w, out_h;
    float jitter_x, jitter_y; // input pixels
    int reset;                // 1: no usable history
    int quality;              // 0 Ultra Quality, 1 Quality, 2 Balanced, 3 Performance
    unsigned preset;          // DLSS render preset hint (1 = A ... 11 = K); 0 = K
    int depth_inverted;       // 1: depth 1 is near
};

extern "C" {
// MUA3_DLSS_API_VERSION of the DLL.
typedef int(__cdecl* Mua3DlssApiVersionFn)();
// Starts NGX on the device and checks DLSS Super Resolution can run. 1 = yes; `status` says why not otherwise.
typedef int(__cdecl* Mua3DlssInitFn)(ID3D11Device* dev, char* status, int status_size);
// Upscales one frame (creates the DLSS feature when the sizes or the quality change). 1 = done.
typedef int(__cdecl* Mua3DlssEvaluateFn)(ID3D11DeviceContext* ctx, const Mua3DlssFrame* frame, char* status, int status_size);
// Releases the feature and NGX.
typedef void(__cdecl* Mua3DlssReleaseFn)();
}
