#include "gpu/d3d11/fsr.h"

#include <bit>
#include <cmath>
#include <cstring>
#include <string_view>
#include <d3dcompiler.h>

#include "ffx_fsr1_embed.h" // (generated: third_party/ffx_fsr1 as byte arrays)

namespace gpu {

namespace {

// The EASU and RCAS passes around AMD's headers (non-packed 32-bit paths: shader model 5).
constexpr const char kSource[] = R"(
#define A_GPU 1
#define A_HLSL 1
#include "ffx_a.h"
Texture2D<float4> tex : register(t0);
SamplerState lin : register(s0);

void vs_main(uint id : SV_VertexID, out float4 pos : SV_Position) {
    float2 uv = float2((id << 1) & 2, id & 2);
    pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}

#if defined(EASU)
cbuffer C : register(b0) { uint4 con0, con1, con2, con3; };
#define FSR_EASU_F 1
AF4 FsrEasuRF(AF2 p) { return tex.GatherRed(lin, p); }
AF4 FsrEasuGF(AF2 p) { return tex.GatherGreen(lin, p); }
AF4 FsrEasuBF(AF2 p) { return tex.GatherBlue(lin, p); }
#include "ffx_fsr1.h"
// Alpha is carried along bilinearly: an upscaled layer the game blends by its alpha (the pause menu's team) keeps its
// coverage (EASU itself only upscales colour).
float4 ps_main(float4 pos : SV_Position) : SV_Target {
    AF3 c;
    FsrEasuF(c, AU2(pos.xy), con0, con1, con2, con3);
    const float2 uv = pos.xy * asfloat(con0.xy) * asfloat(con1.xy); // (output pixel -> input pixel -> input UV)
    return float4(c, tex.SampleLevel(lin, uv, 0).a);
}
#elif defined(COPY_ALPHA)
// Only alpha is written (blend state): the source's, bilinear, into another upscaler's output.
cbuffer C : register(b0) { float4 inv_out; };
float4 ps_main(float4 pos : SV_Position) : SV_Target {
    return float4(0, 0, 0, tex.SampleLevel(lin, pos.xy * inv_out.xy, 0).a);
}
#else
cbuffer C : register(b0) { uint4 con; };
#define FSR_RCAS_F 1
AF4 FsrRcasLoadF(ASU2 p) { return tex.Load(int3(p, 0)); }
void FsrRcasInputF(inout AF1 r, inout AF1 g, inout AF1 b) {}
#include "ffx_fsr1.h"
float4 ps_main(float4 pos : SV_Position) : SV_Target {
    AF1 r, g, b;
    FsrRcasF(r, g, b, AU2(pos.xy), con);
    return float4(r, g, b, tex.Load(int3(pos.xy, 0)).a); // (alpha as EASU left it, unsharpened)
}
#endif
)";

// Serves the two embedded headers to D3DCompile.
struct FfxInclude final : ID3DInclude {
    HRESULT __stdcall Open(D3D_INCLUDE_TYPE, LPCSTR name, LPCVOID, LPCVOID* data, UINT* bytes) override {
        const std::string_view n(name);
        if (n == "ffx_a.h")
            *data = kFfxA, *bytes = UINT(sizeof(kFfxA) - 1);
        else if (n == "ffx_fsr1.h")
            *data = kFfxFsr1, *bytes = UINT(sizeof(kFfxFsr1) - 1);
        else
            return E_FAIL;
        return S_OK;
    }
    HRESULT __stdcall Close(LPCVOID) override { return S_OK; }
};

u32 Bits(float f) { return std::bit_cast<u32>(f); }

} // namespace

bool Fsr::Init(ID3D11Device* dev) {
    tried_ = true;
    FfxInclude inc;
    Microsoft::WRL::ComPtr<ID3DBlob> vs, easu, rcas, err;
    const UINT flags = D3DCOMPILE_OPTIMIZATION_LEVEL3;
    const D3D_SHADER_MACRO easu_def[] = {{"EASU", "1"}, {nullptr, nullptr}};
    const D3D_SHADER_MACRO alpha_def[] = {{"COPY_ALPHA", "1"}, {nullptr, nullptr}};
    Microsoft::WRL::ComPtr<ID3DBlob> alpha;
    if (FAILED(D3DCompile(kSource, sizeof(kSource) - 1, "fsr1", nullptr, &inc, "vs_main", "vs_5_0", flags, 0, &vs, &err)) ||
        FAILED(D3DCompile(kSource, sizeof(kSource) - 1, "fsr1", easu_def, &inc, "ps_main", "ps_5_0", flags, 0, &easu, &err)) ||
        FAILED(D3DCompile(kSource, sizeof(kSource) - 1, "fsr1", nullptr, &inc, "ps_main", "ps_5_0", flags, 0, &rcas, &err)) ||
        FAILED(D3DCompile(kSource, sizeof(kSource) - 1, "fsr1", alpha_def, &inc, "ps_main", "ps_5_0", flags, 0, &alpha, &err))) {
        LOG_ERROR("Renderer", "FSR shaders: {}", err ? std::string(static_cast<const char*>(err->GetBufferPointer()),
                                                                     err->GetBufferSize()) : "compile failed");
        return false;
    }
    if (FAILED(dev->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &vs_)) ||
        FAILED(dev->CreatePixelShader(easu->GetBufferPointer(), easu->GetBufferSize(), nullptr, &easu_)) ||
        FAILED(dev->CreatePixelShader(rcas->GetBufferPointer(), rcas->GetBufferSize(), nullptr, &rcas_)) ||
        FAILED(dev->CreatePixelShader(alpha->GetBufferPointer(), alpha->GetBufferSize(), nullptr, &alpha_)))
        return false;
    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = 64;
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    D3D11_BLEND_DESC bl{};
    bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    D3D11_DEPTH_STENCIL_DESC dd{};
    if (FAILED(dev->CreateSamplerState(&sd, &sampler_)) || FAILED(dev->CreateBuffer(&bd, nullptr, &easu_cb_)) ||
        FAILED(dev->CreateBuffer(&bd, nullptr, &rcas_cb_)) || FAILED(dev->CreateRasterizerState(&rd, &raster_)) ||
        FAILED(dev->CreateBlendState(&bl, &blend_)) || FAILED(dev->CreateDepthStencilState(&dd, &depth_)) ||
        FAILED(dev->CreateBuffer(&bd, nullptr, &alpha_cb_)))
        return false;
    bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALPHA;
    if (FAILED(dev->CreateBlendState(&bl, &alpha_blend_)))
        return false;
    LOG_INFO("Renderer", "FSR 1 (EASU + RCAS) ready");
    return ok_ = true;
}

void Fsr::Draw(ID3D11DeviceContext* ctx, ID3D11PixelShader* ps, ID3D11Buffer* cb, ID3D11ShaderResourceView* src,
               ID3D11RenderTargetView* dst, u32 w, u32 h) {
    ctx->OMSetRenderTargets(1, &dst, nullptr);
    const D3D11_VIEWPORT vp{0, 0, float(w), float(h), 0, 1};
    ctx->RSSetViewports(1, &vp);
    ctx->PSSetShader(ps, nullptr, 0);
    ctx->PSSetConstantBuffers(0, 1, &cb);
    ctx->PSSetShaderResources(0, 1, &src);
    ctx->Draw(3, 0);
    ID3D11ShaderResourceView* null_srv = nullptr;
    ctx->PSSetShaderResources(0, 1, &null_srv);
    ID3D11RenderTargetView* null_rtv = nullptr;
    ctx->OMSetRenderTargets(1, &null_rtv, nullptr);
}

bool Fsr::Run(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* src, u32 in_w, u32 in_h,
              ID3D11RenderTargetView* mid, ID3D11ShaderResourceView* mid_srv, ID3D11RenderTargetView* dst, u32 out_w,
              u32 out_h, float sharpness_stops) {
    if (!tried_)
        Init(dev);
    if (!ok_ || !src || !mid || !mid_srv || !dst || !in_w || !in_h || !out_w || !out_h)
        return false;
    if (in_w != easu_in_w_ || in_h != easu_in_h_ || out_w != easu_out_w_ || out_h != easu_out_h_) {
        // FsrEasuCon (ffx_fsr1.h) on the CPU: the whole input texture is the viewport
        const float iw = float(in_w), ih = float(in_h), ow = float(out_w), oh = float(out_h);
        const u32 c[16] = {Bits(iw / ow), Bits(ih / oh), Bits(0.5f * iw / ow - 0.5f), Bits(0.5f * ih / oh - 0.5f),
                           Bits(1.0f / iw), Bits(1.0f / ih), Bits(1.0f / iw), Bits(-1.0f / ih),
                           Bits(-1.0f / iw), Bits(2.0f / ih), Bits(1.0f / iw), Bits(2.0f / ih),
                           Bits(0.0f / iw), Bits(4.0f / ih), 0, 0};
        ctx->UpdateSubresource(easu_cb_.Get(), 0, nullptr, c, 0, 0);
        easu_in_w_ = in_w, easu_in_h_ = in_h, easu_out_w_ = out_w, easu_out_h_ = out_h;
    }
    if (sharpness_stops != rcas_stops_) { // FsrRcasCon: stops to a linear factor (con[1], the packed-half copy, unused)
        const u32 c[4] = {Bits(std::exp2(-sharpness_stops)), 0, 0, 0};
        ctx->UpdateSubresource(rcas_cb_.Get(), 0, nullptr, c, 0, 0);
        rcas_stops_ = sharpness_stops;
    }
    ctx->RSSetState(raster_.Get());
    ctx->OMSetBlendState(blend_.Get(), nullptr, 0xFFFFFFFF);
    ctx->OMSetDepthStencilState(depth_.Get(), 0);
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(vs_.Get(), nullptr, 0);
    ctx->GSSetShader(nullptr, nullptr, 0);
    ctx->HSSetShader(nullptr, nullptr, 0);
    ctx->DSSetShader(nullptr, nullptr, 0);
    ctx->PSSetSamplers(0, 1, sampler_.GetAddressOf());
    Draw(ctx, easu_.Get(), easu_cb_.Get(), src, mid, out_w, out_h);
    Draw(ctx, rcas_.Get(), rcas_cb_.Get(), mid_srv, dst, out_w, out_h);
    return true;
}

bool Fsr::CopyAlpha(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* src, ID3D11RenderTargetView* dst,
                    u32 out_w, u32 out_h) {
    if (!tried_)
        Init(dev);
    if (!ok_ || !src || !dst || !out_w || !out_h)
        return false;
    if (out_w != alpha_w_ || out_h != alpha_h_) {
        const float c[16] = {1.0f / float(out_w), 1.0f / float(out_h)};
        ctx->UpdateSubresource(alpha_cb_.Get(), 0, nullptr, c, 0, 0);
        alpha_w_ = out_w, alpha_h_ = out_h;
    }
    ctx->RSSetState(raster_.Get());
    ctx->OMSetBlendState(alpha_blend_.Get(), nullptr, 0xFFFFFFFF);
    ctx->OMSetDepthStencilState(depth_.Get(), 0);
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(vs_.Get(), nullptr, 0);
    ctx->GSSetShader(nullptr, nullptr, 0);
    ctx->HSSetShader(nullptr, nullptr, 0);
    ctx->DSSetShader(nullptr, nullptr, 0);
    ctx->PSSetSamplers(0, 1, sampler_.GetAddressOf());
    Draw(ctx, alpha_.Get(), alpha_cb_.Get(), src, dst, out_w, out_h);
    return true;
}

} // namespace gpu
