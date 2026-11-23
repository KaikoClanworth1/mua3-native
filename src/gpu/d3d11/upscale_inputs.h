#pragma once
// What every temporal upscaler (DLSS, FSR 3, XeSS) needs from a frame, built from what the game draws:
// - the scene depth: the depth target of its depth pre-pass (screen-sized, no colour targets);
// - the view-projection matrix: the first 64 bytes of that pass's vertex constant buffer 3 (row vectors:
//   clip = (x, y, z, 1) * M), this frame's and the last;
// - motion vectors from those: camera motion only (current -> previous position, in pixels, RG16F at the depth's
//   size). Moving characters get the motion of whatever is behind them, as the game has no velocity buffer.
// MUA3_UPSCALE_DEBUG=1 shows the depth, 2 the motion vectors, over the frame shown.
#include <unordered_map>
#include <d3d11.h>
#include <wrl/client.h>

#include "common/common.h"

namespace gpu {

class UpscaleInputs {
public:
    // 0 off, 1 depth, 2 motion vectors (MUA3_UPSCALE_DEBUG).
    static int DebugMode();

    // Every draw into a frame-sized depth votes with the first 64 bytes of its vertex constant buffer 3; Resolve()
    // takes the matrix most draws share (the pre-pass and the skinned programs keep the camera there; other programs
    // keep per-object data) as the camera, and that depth as the scene's. `flip`, `sz`, `tz` map the clip position to
    // the depth stored and the pixel row, as the vertex shader does (drv_viewport): y *= flip, depth = z/w * sz + tz.
    void Vote(u64 depth_va, const float vp[16], float flip, float sz, float tz);
    void Resolve(); // (once the scene is drawn: at the composite or at present)
    u32 stat_draws = 0, stat_read = 0; // this frame: draws into a frame-sized depth, those whose buffer was read
    bool Noted() const { return noted_; }
    u64 DepthVa() const { return depth_va_; }

    // Camera motion vectors from `depth` (w x h, the scene depth) into the texture Motion() returns.
    // `object_motion` (optional): the velocity variants' motion at the same size, used where written (not 65504).
    bool BuildMotion(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* depth, u32 w, u32 h,
                     ID3D11ShaderResourceView* object_motion = nullptr);
    ID3D11ShaderResourceView* Motion() const { return mv_srv_.Get(); }
    ID3D11Texture2D* MotionTex() const { return mv_tex_.Get(); }

    // The debug view into `dst` (w x h): the scene (`scene`, sampled) dimmed under the depth or the motion vectors.
    bool DrawDebug(ID3D11Device* dev, ID3D11DeviceContext* ctx, int mode, ID3D11ShaderResourceView* scene,
                   ID3D11ShaderResourceView* depth, ID3D11RenderTargetView* dst, u32 w, u32 h);

    // From this frame's camera (after Resolve): device depth -> view distance, dist = [1] / (depth - [0]), and the
    // tangents of the half field of view in x ([2]) and y ([3]), as FSR 3 takes them. False without a camera.
    bool DepthToView(float out[4]) const;

    // At the end of a frame: this frame's matrix becomes the previous one.
    void EndFrame();
    void Reset() { *this = UpscaleInputs{}; }

private:
    bool Init(ID3D11Device* dev);
    void Upload(ID3D11DeviceContext* ctx, u32 w, u32 h, float mode, bool object_motion = false);
    bool tried_ = false, ok_ = false;
    bool noted_ = false, have_prev_ = false;
    struct Candidate {
        float vp[16];
        u64 depth_va = 0;
        float flip = 1, sz = 1, tz = 0;
        u32 n = 1;
    };
    std::unordered_map<u64, Candidate> votes_;
    bool resolved_ = false;
    bool logged_camera_ = false;
    u64 resolves_ = 0;
    u64 depth_va_ = 0;
    float vp_[16]{}, prev_vp_[16]{};
    float flip_ = 1, sz_ = 1, tz_ = 0;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vs_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> mv_ps_, debug_ps_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> cb_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler_;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> raster_;
    Microsoft::WRL::ComPtr<ID3D11BlendState> blend_;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depth_state_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> mv_tex_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> mv_rtv_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> mv_srv_;
    u32 mv_w_ = 0, mv_h_ = 0;
};

} // namespace gpu
