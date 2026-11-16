#include "gpu/d3d11/upscale_inputs.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <d3dcompiler.h>

namespace gpu {

namespace {

constexpr const char kSource[] = R"(
Texture2D<float> depth_tex : register(t0);
Texture2D<float4> scene : register(t1);
SamplerState lin : register(s0);
cbuffer C : register(b0) {
    row_major float4x4 inv_vp;  // this frame's clip -> world (homogeneous)
    row_major float4x4 prev_vp; // last frame's world -> clip
    float4 vpz;   // flip, depth scale, depth offset, debug mode
    float4 size;  // target w, h, 1/w, 1/h
    float4 dsize; // depth w, h, 1/w, 1/h
};

void vs_main(uint id : SV_VertexID, out float4 pos : SV_Position) {
    float2 uv = float2((id << 1) & 2, id & 2);
    pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}

// The world position (homogeneous, not divided: points at infinity stay exact) of depth texel `px`.
float4 WorldAt(float2 px, out float2 uv) {
    uv = px * dsize.zw;
    const float d = depth_tex.Load(int3(px, 0));
    const float4 clip = float4(uv.x * 2 - 1, (1 - uv.y * 2) * vpz.x, (d - vpz.z) / vpz.y, 1);
    return mul(clip, inv_vp);
}

// Where the point at depth texel `px` was last frame, in pixels from where it is now (current -> previous).
float2 MotionAt(float2 px) {
    float2 uv;
    const float4 pc = mul(WorldAt(px, uv), prev_vp);
    const float2 pn = pc.xy / pc.w;
    const float2 puv = float2(pn.x * 0.5 + 0.5, 0.5 - pn.y * vpz.x * 0.5);
    return (puv - uv) * dsize.xy;
}

float2 ps_motion(float4 pos : SV_Position) : SV_Target {
    return MotionAt(pos.xy);
}

float4 ps_debug(float4 pos : SV_Position) : SV_Target {
    const float2 uv = pos.xy * size.zw;
    const float2 px = floor(uv * dsize.xy) + 0.5;
    const float luma = dot(scene.SampleLevel(lin, uv, 0).rgb, float3(0.299, 0.587, 0.114));
    if (vpz.w < 1.5) { // depth: distance from the camera (clip w = 1 / world w), log scale; cleared depth blue
        float2 u;
        const float4 wh = WorldAt(px, u);
        const float d = depth_tex.Load(int3(px, 0));
        if (d <= 0.0 || d >= 1.0)
            return float4(0.1, 0.2, 0.6, 1);
        const float g = 1 - saturate(log2(max(1 / max(abs(wh.w), 1e-20), 1)) / 18);
        return float4(g.xxx, 1);
    }
    // motion: red right / cyan left, green down / magenta up, 16 px for full colour; the scene faintly beneath
    const float2 m = clamp(MotionAt(px) / 16, -1, 1);
    const float3 c = float3(0.5 + 0.5 * m.x, 0.5 + 0.5 * m.y, 0.5 - 0.25 * (m.x + m.y));
    return float4(lerp(c, luma.xxx, 0.25), 1);
}
)";

// 4x4 inverse (cofactors); false if singular. The layout does not matter (inverse of the transpose = transpose).
bool Invert(const float m[16], float out[16]) {
    double inv[16];
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
    double det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (!std::isfinite(det) || std::abs(det) < 1e-30)
        return false;
    det = 1.0 / det;
    for (int i = 0; i < 16; i++)
        out[i] = float(inv[i] * det);
    return true;
}

struct Constants {
    float inv_vp[16], prev_vp[16];
    float vpz[4], size[4], dsize[4];
    float pad[4];
};

} // namespace

int UpscaleInputs::DebugMode() {
    static const int mode = [] {
        const char* e = std::getenv("MUA3_UPSCALE_DEBUG");
        return e ? std::atoi(e) : 0;
    }();
    return mode;
}

void UpscaleInputs::Vote(u64 depth_va, const float vp[16], float flip, float sz, float tz) {
    if (resolved_)
        return;
    for (int i = 0; i < 16; i++)
        if (!std::isfinite(vp[i]))
            return;
    // A camera's view-projection: a perspective (w) column, w translated by the camera's distance (vp[15]: a
    // world origin exactly at the camera is not this game), and invertible. Shared constants (e.g. 0 0 1 0 rows) fail.
    if ((vp[3] == 0 && vp[7] == 0 && vp[11] == 0) || std::abs(vp[15]) < 1e-3f)
        return;
    if (float inv[16]; !Invert(vp, inv))
        return;
    u64 h = 0xcbf29ce484222325ull ^ depth_va;
    const u8* b = reinterpret_cast<const u8*>(vp);
    for (int i = 0; i < 64; i++)
        h = (h ^ b[i]) * 0x100000001b3ull;
    auto it = votes_.find(h);
    if (it != votes_.end()) {
        it->second.n++;
        return;
    }
    if (votes_.size() >= 4096)
        return;
    Candidate c;
    std::memcpy(c.vp, vp, sizeof(c.vp));
    c.depth_va = depth_va, c.flip = flip, c.sz = sz, c.tz = tz;
    votes_.emplace(h, c);
}

void UpscaleInputs::Resolve() {
    if (resolved_)
        return;
    resolved_ = true;
    const Candidate* best = nullptr;
    for (const auto& [h, c] : votes_)
        if (!best || c.n > best->n)
            best = &c;
    const bool log = stat_draws && ++resolves_ % (logged_camera_ ? 3000 : 120) == 1;
    if (log) { // (the runner-up too: the camera should win clearly)
        const Candidate* second = nullptr;
        for (const auto& [h, c] : votes_)
            if (&c != best && (!second || c.n > second->n))
                second = &c;
        LOG_INFO("Renderer", "upscaler inputs: {} draws into the scene depth, {} buffers read, {} candidates; best {} votes ({:g} {:g} {:g} {:g} / {:g} {:g} {:g} {:g} / {:g} {:g} {:g} {:g} / {:g} {:g} {:g} {:g}), next {}",
                 stat_draws, stat_read, votes_.size(), best ? best->n : 0, best ? best->vp[0] : 0, best ? best->vp[1] : 0,
                 best ? best->vp[2] : 0, best ? best->vp[3] : 0, best ? best->vp[4] : 0, best ? best->vp[5] : 0, best ? best->vp[6] : 0,
                 best ? best->vp[7] : 0, best ? best->vp[8] : 0, best ? best->vp[9] : 0, best ? best->vp[10] : 0, best ? best->vp[11] : 0,
                 best ? best->vp[12] : 0, best ? best->vp[13] : 0, best ? best->vp[14] : 0, best ? best->vp[15] : 0, second ? second->n : 0);
    }
    if (!best || best->n < 4) // (per-object data repeats rarely: the camera is shared by many draws)
        return;
    if (log && !logged_camera_) {
        logged_camera_ = true;
        LOG_INFO("Renderer", "upscaler inputs: scene depth {:#x}", best->depth_va);
    }
    std::memcpy(vp_, best->vp, sizeof(vp_));
    depth_va_ = best->depth_va;
    flip_ = best->flip, sz_ = best->sz, tz_ = best->tz;
    noted_ = true;
}

void UpscaleInputs::EndFrame() {
    if (noted_) {
        std::memcpy(prev_vp_, vp_, sizeof(prev_vp_));
        have_prev_ = true;
    }
    noted_ = resolved_ = false;
    votes_.clear();
    stat_draws = stat_read = 0;
}

bool UpscaleInputs::Init(ID3D11Device* dev) {
    tried_ = true;
    Microsoft::WRL::ComPtr<ID3DBlob> vs, mv, dbg, err;
    const UINT flags = D3DCOMPILE_OPTIMIZATION_LEVEL3;
    if (FAILED(D3DCompile(kSource, sizeof(kSource) - 1, "upscale_inputs", nullptr, nullptr, "vs_main", "vs_5_0", flags, 0, &vs, &err)) ||
        FAILED(D3DCompile(kSource, sizeof(kSource) - 1, "upscale_inputs", nullptr, nullptr, "ps_motion", "ps_5_0", flags, 0, &mv, &err)) ||
        FAILED(D3DCompile(kSource, sizeof(kSource) - 1, "upscale_inputs", nullptr, nullptr, "ps_debug", "ps_5_0", flags, 0, &dbg, &err))) {
        LOG_ERROR("Renderer", "upscaler input shaders: {}",
                  err ? std::string(static_cast<const char*>(err->GetBufferPointer()), err->GetBufferSize()) : "compile failed");
        return false;
    }
    if (FAILED(dev->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &vs_)) ||
        FAILED(dev->CreatePixelShader(mv->GetBufferPointer(), mv->GetBufferSize(), nullptr, &mv_ps_)) ||
        FAILED(dev->CreatePixelShader(dbg->GetBufferPointer(), dbg->GetBufferSize(), nullptr, &debug_ps_)))
        return false;
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = sizeof(Constants);
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    D3D11_BLEND_DESC bl{};
    bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    D3D11_DEPTH_STENCIL_DESC dd{};
    if (FAILED(dev->CreateBuffer(&bd, nullptr, &cb_)) || FAILED(dev->CreateSamplerState(&sd, &sampler_)) ||
        FAILED(dev->CreateRasterizerState(&rd, &raster_)) || FAILED(dev->CreateBlendState(&bl, &blend_)) ||
        FAILED(dev->CreateDepthStencilState(&dd, &depth_state_)))
        return false;
    LOG_INFO("Renderer", "upscaler inputs ready (camera motion vectors from depth{})",
             DebugMode() ? fmt::format(", debug view {}", DebugMode() == 1 ? "depth" : "motion vectors") : "");
    return ok_ = true;
}

void UpscaleInputs::Upload(ID3D11DeviceContext* ctx, u32 w, u32 h, float mode) {
    Constants c{};
    if (!Invert(vp_, c.inv_vp))
        std::memset(c.inv_vp, 0, sizeof(c.inv_vp));
    std::memcpy(c.prev_vp, have_prev_ ? prev_vp_ : vp_, sizeof(c.prev_vp));
    c.vpz[0] = flip_, c.vpz[1] = sz_ != 0 ? sz_ : 1, c.vpz[2] = tz_, c.vpz[3] = mode;
    c.size[0] = float(w), c.size[1] = float(h), c.size[2] = 1.0f / float(w), c.size[3] = 1.0f / float(h);
    c.dsize[0] = float(mv_w_), c.dsize[1] = float(mv_h_), c.dsize[2] = 1.0f / float(mv_w_), c.dsize[3] = 1.0f / float(mv_h_);
    ctx->UpdateSubresource(cb_.Get(), 0, nullptr, &c, 0, 0);
    ctx->RSSetState(raster_.Get());
    ctx->OMSetBlendState(blend_.Get(), nullptr, 0xFFFFFFFF);
    ctx->OMSetDepthStencilState(depth_state_.Get(), 0);
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(vs_.Get(), nullptr, 0);
    ctx->GSSetShader(nullptr, nullptr, 0);
    ctx->HSSetShader(nullptr, nullptr, 0);
    ctx->DSSetShader(nullptr, nullptr, 0);
    ctx->PSSetConstantBuffers(0, 1, cb_.GetAddressOf());
    ctx->PSSetSamplers(0, 1, sampler_.GetAddressOf());
    const D3D11_VIEWPORT vp{0, 0, float(w), float(h), 0, 1};
    ctx->RSSetViewports(1, &vp);
}

bool UpscaleInputs::BuildMotion(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* depth, u32 w, u32 h) {
    if (!tried_)
        Init(dev);
    if (!ok_ || !noted_ || !depth || !w || !h)
        return false;
    if (!mv_tex_ || mv_w_ != w || mv_h_ != h) {
        mv_tex_.Reset(), mv_rtv_.Reset(), mv_srv_.Reset();
        D3D11_TEXTURE2D_DESC td{};
        td.Width = w;
        td.Height = h;
        td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R16G16_FLOAT;
        td.SampleDesc.Count = 1;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        if (FAILED(dev->CreateTexture2D(&td, nullptr, &mv_tex_)) || FAILED(dev->CreateRenderTargetView(mv_tex_.Get(), nullptr, &mv_rtv_)) ||
            FAILED(dev->CreateShaderResourceView(mv_tex_.Get(), nullptr, &mv_srv_))) {
            mv_tex_.Reset();
            return false;
        }
        mv_w_ = w, mv_h_ = h;
        LOG_INFO("Renderer", "motion vectors {}x{} (camera only)", w, h);
    }
    Upload(ctx, w, h, 0);
    ID3D11ShaderResourceView* null_srv[2] = {};
    ctx->PSSetShaderResources(0, 2, null_srv);
    ID3D11RenderTargetView* rtv = mv_rtv_.Get();
    ctx->OMSetRenderTargets(1, &rtv, nullptr);
    ctx->PSSetShader(mv_ps_.Get(), nullptr, 0);
    ctx->PSSetShaderResources(0, 1, &depth);
    ctx->Draw(3, 0);
    ctx->PSSetShaderResources(0, 1, null_srv);
    ID3D11RenderTargetView* null_rtv = nullptr;
    ctx->OMSetRenderTargets(1, &null_rtv, nullptr);
    return true;
}

bool UpscaleInputs::DrawDebug(ID3D11Device* dev, ID3D11DeviceContext* ctx, int mode, ID3D11ShaderResourceView* scene,
                              ID3D11ShaderResourceView* depth, ID3D11RenderTargetView* dst, u32 w, u32 h) {
    if (!ok_ || !noted_ || !mv_tex_ || !scene || !depth || !dst)
        return false;
    Upload(ctx, w, h, float(mode));
    ctx->OMSetRenderTargets(1, &dst, nullptr);
    ctx->PSSetShader(debug_ps_.Get(), nullptr, 0);
    ID3D11ShaderResourceView* srvs[2] = {depth, scene};
    ctx->PSSetShaderResources(0, 2, srvs);
    ctx->Draw(3, 0);
    ID3D11ShaderResourceView* null_srv[2] = {};
    ctx->PSSetShaderResources(0, 2, null_srv);
    ID3D11RenderTargetView* null_rtv = nullptr;
    ctx->OMSetRenderTargets(1, &null_rtv, nullptr);
    return true;
}

} // namespace gpu
