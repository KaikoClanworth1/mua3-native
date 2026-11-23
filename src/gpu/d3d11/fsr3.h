#pragma once
// AMD FidelityFX Super Resolution 3.1 upscaler (mua3.ini / Graphics menu: upscaler=3), on D3D11: AMD's compute passes
// (third_party/ffx_fsr3, MIT; compiled by fxc at build time, cs_5_0) driven here instead of through the FidelityFX
// API, which has no D3D11 backend. It takes what DLSS takes (UpscaleInputs): the jittered scene, its depth and the
// motion vectors (pixels, current -> previous, at the render size), and accumulates at the output size.
// Needs a feature level 11_1 device (the luma pyramid binds 9 UAVs) with typed UAV loads of RG16F and RGBA32F.
#include <string>
#include <d3d11.h>
#include <wrl/client.h>

#include "common/common.h"

namespace gpu {

class Fsr3 {
public:
    struct Frame {
        ID3D11ShaderResourceView* color = nullptr;  // the jittered scene (in_w x in_h)
        ID3D11ShaderResourceView* depth = nullptr;  // its depth (0 near, 1 far)
        ID3D11ShaderResourceView* motion = nullptr; // pixels, current -> previous (RG)
        ID3D11Texture2D* output = nullptr;          // out_w x out_h, UAV-capable (RGBA16F)
        u32 in_w = 0, in_h = 0, out_w = 0, out_h = 0;
        float jitter_x = 0, jitter_y = 0; // pixels, as the viewport was offset
        // Device depth -> view distance: dist = [1] / (depth - [0]); [2], [3]: tan of the half field of view in x, y.
        float depth_to_view[4] = {};
        float sharpness = 0.8f; // 0..1 (RCAS); 0 skips it
        float frame_ms = 16.7f;
        bool reset = false;
    };

    bool Available(ID3D11Device* dev); // (the first call creates the shaders)
    const std::string& Status() const { return status_; }
    // False if FSR 3 could not run (the caller falls back). Leaves the context's state changed (ContextEpoch).
    bool Evaluate(ID3D11Device* dev, ID3D11DeviceContext* ctx, const Frame& f);
    void Reset() { *this = Fsr3{}; }

private:
    using Tex = Microsoft::WRL::ComPtr<ID3D11Texture2D>;
    using Srv = Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>;
    using Uav = Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView>;
    struct Target {
        Tex tex;
        Srv srv;
        Uav uav;
    };
    bool Init(ID3D11Device* dev);
    bool Resize(ID3D11Device* dev, u32 in_w, u32 in_h, u32 out_w, u32 out_h);
    bool Make(ID3D11Device* dev, Target& t, u32 w, u32 h, DXGI_FORMAT fmt, const void* init = nullptr, u32 pitch = 0);

    bool tried_ = false, ok_ = false;
    std::string status_ = "not tried";
    enum Pass { kPrepareInputs, kLumaPyramid, kShadingChangePyramid, kShadingChange, kPrepareReactivity,
                kLumaInstability, kAccumulate, kAccumulateSharpen, kRcas, kPassCount };
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> cs_[kPassCount];
    Microsoft::WRL::ComPtr<ID3D11Buffer> cb_main_, cb_spd_, cb_rcas_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> point_, linear_;
    Target default_exposure_, default_reactive_, lanczos_lut_, frame_info_, spd_atomic_;

    // Render-size targets
    Target dilated_depth_, dilated_mv_, recon_prev_depth_, farthest_depth_ /* (also luma instability) */,
        farthest_depth_mip1_, dilated_reactive_, accumulation_[2], luma_[2], luma_history_[2];
    Target shading_change_; // half the render size
    Tex spd_tex_;           // half the render size, all mips (RG16F)
    Srv spd_srv_;
    Uav spd_uav_[6];
    // Output-size targets
    Target upscaled_[2], new_locks_;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> out_uav_;
    ID3D11Texture2D* out_tex_ = nullptr;
    u32 in_w_ = 0, in_h_ = 0, out_w_ = 0, out_h_ = 0;

    // Per-frame state (as AMD's context keeps it)
    u32 frame_ = 0;         // ping-pong index
    bool first_ = true;
    float jitter_[2] = {}, prev_jitter_[2] = {};
    int prev_render_[2] = {}, prev_upscale_[2] = {};
    float jitter_phases_ = 0, frame_index_ = 0;
};

} // namespace gpu
