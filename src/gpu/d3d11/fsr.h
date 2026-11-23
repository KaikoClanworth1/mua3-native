#pragma once
// AMD FidelityFX Super Resolution 1 (mua3.ini / Graphics menu: upscaler=1): EASU upscales the finished frame from the
// internal render size to the output size, RCAS sharpens it. The shader headers are AMD's (third_party/ffx_fsr1, MIT),
// compiled at runtime. Both passes work on the frame's raw (gamma-encoded) values, as FSR 1 expects.
#include <d3d11.h>
#include <wrl/client.h>

#include "common/common.h"

namespace gpu {

class Fsr {
public:
    // EASU from `src` (in_w x in_h) into `mid` (out_w x out_h), then RCAS from `mid_srv` into `dst`.
    // `sharpness_stops`: RCAS strength, 0 = maximum, each stop halves it. False if the passes are unavailable.
    // Leaves the context's state changed: the caller tells its state caches (ContextEpoch).
    bool Run(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* src, u32 in_w, u32 in_h,
             ID3D11RenderTargetView* mid, ID3D11ShaderResourceView* mid_srv, ID3D11RenderTargetView* dst, u32 out_w,
             u32 out_h, float sharpness_stops);
    // Writes only alpha into `dst` (out_w x out_h): `src`'s, sampled bilinearly. For the temporal upscalers' output (FSR
    // 3.1, DLSS), which does not keep the layer's alpha the game may blend by (the pause menu's team).
    bool CopyAlpha(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* src, ID3D11RenderTargetView* dst,
                   u32 out_w, u32 out_h);

private:
    bool Init(ID3D11Device* dev);
    void Draw(ID3D11DeviceContext* ctx, ID3D11PixelShader* ps, ID3D11Buffer* cb, ID3D11ShaderResourceView* src,
              ID3D11RenderTargetView* dst, u32 w, u32 h);
    bool tried_ = false, ok_ = false;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vs_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> easu_, rcas_, alpha_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> easu_cb_, rcas_cb_, alpha_cb_;
    Microsoft::WRL::ComPtr<ID3D11BlendState> alpha_blend_;
    u32 alpha_w_ = 0, alpha_h_ = 0;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> raster_;
    Microsoft::WRL::ComPtr<ID3D11BlendState> blend_;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depth_;
    u32 easu_in_w_ = 0, easu_in_h_ = 0, easu_out_w_ = 0, easu_out_h_ = 0;
    float rcas_stops_ = -1;
};

} // namespace gpu
