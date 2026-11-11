#pragma once
// Anti-aliasing passes on the presented frame (mua3.ini / in-game Graphics menu: anti_aliasing): FXAA, and the
// resolve of a supersampled frame (SSAA renders the screen-sized targets at a multiple of the render scale).
#include <d3d11.h>
#include <wrl/client.h>

#include "common/common.h"

namespace gpu {

class PostAA {
public:
    // FXAA from `src` (the frame as the game drew it) into `dst` (same size). False if the pass is unavailable
    // (shader build failed): the caller copies the frame instead. Leaves the context's state changed: the caller
    // tells its state caches (ContextEpoch).
    bool Fxaa(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* src, ID3D11RenderTargetView* dst,
              u32 width, u32 height, bool srgb);
    // Supersampling resolve: `src` is `factor` times `width` x `height`; each output pixel averages its texels.
    bool Downsample(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* src, ID3D11RenderTargetView* dst,
                    u32 width, u32 height, bool srgb, u32 factor);

private:
    bool Pass(ID3D11Device* dev, ID3D11DeviceContext* ctx, bool down, ID3D11ShaderResourceView* src,
              ID3D11RenderTargetView* dst, u32 width, u32 height, bool srgb, u32 factor);
    bool Init(ID3D11Device* dev);
    bool tried_ = false, ok_ = false;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vs_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> ps_, down_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> cb_;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> raster_;
    Microsoft::WRL::ComPtr<ID3D11BlendState> blend_;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depth_;
    u32 cb_w_ = 0, cb_h_ = 0, cb_srgb_ = 2, cb_factor_ = 0;
    struct Timer { // (MUA3_AA_TIME)
        Microsoft::WRL::ComPtr<ID3D11Query> disjoint, begin, end;
        bool pending = false;
        const char* name = "";
    };
    void ReadTimer(ID3D11DeviceContext* ctx, Timer& t);
    Timer timers_[4];
    u32 timer_next_ = 0, time_n_ = 0;
    double time_sum_ = 0;
};

} // namespace gpu
