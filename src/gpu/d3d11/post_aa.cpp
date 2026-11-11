#include "gpu/d3d11/post_aa.h"

#include <cstdlib>
#include <cstring>
#include <d3dcompiler.h>

namespace gpu {

namespace {

// FXAA (after Timothy Lottes' FXAA 3.11 "quality" algorithm): find edges by local luma contrast, search along each
// edge for its ends, and resample across it in proportion to the pixel's distance from the nearer end; plus a
// sub-pixel term for single-pixel features. Luma from gamma-space colour (sqrt of linear for sRGB surfaces).
constexpr const char kSource[] = R"(
cbuffer P : register(b0) { float2 rcp_size; float srgb; float factor; };
Texture2D<float4> tex : register(t0);
SamplerState lin : register(s0);

void vs_main(uint id : SV_VertexID, out float4 pos : SV_Position, out float2 uv : TEXCOORD0) {
    uv = float2((id << 1) & 2, id & 2);
    pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}

float Luma(float3 c) {
    c = srgb > 0.5 ? sqrt(c) : c;
    return dot(c, float3(0.299, 0.587, 0.114));
}
float LumaAt(float2 uv) { return Luma(tex.SampleLevel(lin, uv, 0).rgb); }

static const float kSteps[10] = {1.0, 1.5, 2.0, 2.0, 2.0, 2.0, 2.0, 4.0, 8.0, 8.0};
static const float kEdgeThreshold = 0.125;    // minimum contrast, relative to the local maximum
static const float kEdgeThresholdMin = 0.0312; // and absolute (dark areas)
static const float kSubpix = 0.5;             // sub-pixel smoothing (FXAA's default 0.75 softens text more)

float4 ps_main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    const float4 rgbM = tex.SampleLevel(lin, uv, 0);
    const float lM = Luma(rgbM.rgb);
    float lN = Luma(tex.SampleLevel(lin, uv, 0, int2(0, -1)).rgb);
    float lS = Luma(tex.SampleLevel(lin, uv, 0, int2(0, 1)).rgb);
    const float lW = Luma(tex.SampleLevel(lin, uv, 0, int2(-1, 0)).rgb);
    const float lE = Luma(tex.SampleLevel(lin, uv, 0, int2(1, 0)).rgb);
    const float lMax = max(max(max(lN, lS), max(lW, lE)), lM);
    const float lMin = min(min(min(lN, lS), min(lW, lE)), lM);
    const float range = lMax - lMin;
    if (range < max(kEdgeThresholdMin, lMax * kEdgeThreshold))
        return rgbM;

    const float lNW = Luma(tex.SampleLevel(lin, uv, 0, int2(-1, -1)).rgb);
    const float lNE = Luma(tex.SampleLevel(lin, uv, 0, int2(1, -1)).rgb);
    const float lSW = Luma(tex.SampleLevel(lin, uv, 0, int2(-1, 1)).rgb);
    const float lSE = Luma(tex.SampleLevel(lin, uv, 0, int2(1, 1)).rgb);

    // Edge direction: a horizontal edge changes luma vertically.
    const float edgeH = abs(lNW - 2 * lW + lSW) + 2 * abs(lN - 2 * lM + lS) + abs(lNE - 2 * lE + lSE);
    const float edgeV = abs(lNW - 2 * lN + lNE) + 2 * abs(lW - 2 * lM + lE) + abs(lSW - 2 * lS + lSE);
    const bool horz = edgeH >= edgeV;

    // Sub-pixel blend from the 3x3 average.
    const float avg = (2 * (lN + lS + lW + lE) + (lNW + lNE + lSW + lSE)) / 12.0;
    const float subA = saturate(abs(avg - lM) / range);
    const float subB = (-2 * subA + 3) * subA * subA;
    const float subH = subB * subB * kSubpix;

    if (!horz) {
        lN = lW;
        lS = lE;
    }
    float stepLen = horz ? rcp_size.y : rcp_size.x;
    const float gradN = abs(lN - lM), gradS = abs(lS - lM);
    const bool pairN = gradN >= gradS;
    const float gradient = max(gradN, gradS);
    if (pairN)
        stepLen = -stepLen;
    const float localAvg = 0.5 * ((pairN ? lN : lS) + lM);

    // Walk both ways along the edge, on the boundary between the pixel and its neighbour across it.
    float2 posB = uv;
    if (horz) posB.y += stepLen * 0.5; else posB.x += stepLen * 0.5;
    const float2 dir = horz ? float2(rcp_size.x, 0) : float2(0, rcp_size.y);
    float2 posN = posB - dir, posP = posB + dir;
    const float gradScaled = gradient * 0.25;
    float endN = LumaAt(posN) - localAvg, endP = LumaAt(posP) - localAvg;
    bool doneN = abs(endN) >= gradScaled, doneP = abs(endP) >= gradScaled;
    [loop] for (int i = 1; i < 10 && !(doneN && doneP); i++) {
        if (!doneN) {
            posN -= dir * kSteps[i];
            endN = LumaAt(posN) - localAvg;
            doneN = abs(endN) >= gradScaled;
        }
        if (!doneP) {
            posP += dir * kSteps[i];
            endP = LumaAt(posP) - localAvg;
            doneP = abs(endP) >= gradScaled;
        }
    }
    const float dstN = horz ? uv.x - posN.x : uv.y - posN.y;
    const float dstP = horz ? posP.x - uv.x : posP.y - uv.y;
    const bool nearN = dstN < dstP;
    const bool mBelow = lM - localAvg < 0;
    const bool goodSpan = ((nearN ? endN : endP) < 0) != mBelow;
    const float offset = goodSpan ? 0.5 - min(dstN, dstP) / (dstN + dstP) : 0;
    const float amount = max(offset, subH);
    float2 at = uv;
    if (horz) at.y += amount * stepLen; else at.x += amount * stepLen;
    return float4(tex.SampleLevel(lin, at, 0).rgb, rgbM.a);
}

// Supersampling resolve: the average of the factor x factor source texels under each output pixel (an ordered-grid
// box filter, exact for integer factors), in linear light (gamma-encoded surfaces are decoded and encoded again).
float3 ToLinear(float3 c) { return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4); }
float3 ToGamma(float3 c) { return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1.0 / 2.4) - 0.055; }
float4 ps_down(float4 pos : SV_Position) : SV_Target {
    const int n = int(factor);
    const int2 base = int2(pos.xy) * n;
    float4 sum = 0;
    [loop] for (int y = 0; y < n; y++)
        [loop] for (int x = 0; x < n; x++) {
            float4 c = tex.Load(int3(base + int2(x, y), 0));
            if (srgb < 0.5)
                c.rgb = ToLinear(saturate(c.rgb));
            sum += c;
        }
    sum /= float(n * n);
    if (srgb < 0.5)
        sum.rgb = ToGamma(sum.rgb);
    return sum;
}
)";

struct Params {
    float rcp_w, rcp_h, srgb, factor;
};

} // namespace

bool PostAA::Init(ID3D11Device* dev) {
    tried_ = true;
    Microsoft::WRL::ComPtr<ID3DBlob> vs, ps, down, err;
    const UINT flags = D3DCOMPILE_OPTIMIZATION_LEVEL3;
    if (FAILED(D3DCompile(kSource, sizeof(kSource) - 1, "fxaa", nullptr, nullptr, "vs_main", "vs_5_0", flags, 0, &vs, &err)) ||
        FAILED(D3DCompile(kSource, sizeof(kSource) - 1, "fxaa", nullptr, nullptr, "ps_main", "ps_5_0", flags, 0, &ps, &err)) ||
        FAILED(D3DCompile(kSource, sizeof(kSource) - 1, "fxaa", nullptr, nullptr, "ps_down", "ps_5_0", flags, 0, &down, &err))) {
        LOG_ERROR("Renderer", "AA shaders: {}", err ? std::string(static_cast<const char*>(err->GetBufferPointer()),
                                                                   err->GetBufferSize()) : "compile failed");
        return false;
    }
    if (FAILED(dev->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &vs_)) ||
        FAILED(dev->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &ps_)) ||
        FAILED(dev->CreatePixelShader(down->GetBufferPointer(), down->GetBufferSize(), nullptr, &down_)))
        return false;
    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = sizeof(Params);
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    D3D11_BLEND_DESC bl{};
    bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    D3D11_DEPTH_STENCIL_DESC dd{};
    if (FAILED(dev->CreateSamplerState(&sd, &sampler_)) || FAILED(dev->CreateBuffer(&bd, nullptr, &cb_)) ||
        FAILED(dev->CreateRasterizerState(&rd, &raster_)) || FAILED(dev->CreateBlendState(&bl, &blend_)) ||
        FAILED(dev->CreateDepthStencilState(&dd, &depth_)))
        return false;
    LOG_INFO("Renderer", "post AA passes ready");
    return ok_ = true;
}

void PostAA::ReadTimer(ID3D11DeviceContext* ctx, Timer& t) {
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
    UINT64 b = 0, e = 0;
    if (ctx->GetData(t.disjoint.Get(), &dj, sizeof(dj), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
        ctx->GetData(t.begin.Get(), &b, sizeof(b), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
        ctx->GetData(t.end.Get(), &e, sizeof(e), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
        return; // (not ready: this slot is reused anyway)
    t.pending = false;
    if (dj.Disjoint || !dj.Frequency)
        return;
    time_sum_ += double(e - b) * 1000.0 / double(dj.Frequency);
    if (++time_n_ == 300) {
        LOG_INFO("Renderer", "{} pass: {:.3f} ms GPU time per frame", t.name, time_sum_ / time_n_);
        time_sum_ = 0;
        time_n_ = 0;
    }
}

bool PostAA::Fxaa(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* src, ID3D11RenderTargetView* dst,
                  u32 width, u32 height, bool srgb) {
    return Pass(dev, ctx, false, src, dst, width, height, srgb, 1);
}

bool PostAA::Downsample(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* src,
                        ID3D11RenderTargetView* dst, u32 width, u32 height, bool srgb, u32 factor) {
    return Pass(dev, ctx, true, src, dst, width, height, srgb, factor);
}

bool PostAA::Pass(ID3D11Device* dev, ID3D11DeviceContext* ctx, bool down, ID3D11ShaderResourceView* src,
                  ID3D11RenderTargetView* dst, u32 width, u32 height, bool srgb, u32 factor) {
    if (!tried_)
        Init(dev);
    if (!ok_ || !src || !dst || !width || !height)
        return false;
    if (width != cb_w_ || height != cb_h_ || u32(srgb) != cb_srgb_ || factor != cb_factor_) {
        const Params p{1.0f / width, 1.0f / height, srgb ? 1.0f : 0.0f, float(factor)};
        ctx->UpdateSubresource(cb_.Get(), 0, nullptr, &p, 0, 0);
        cb_w_ = width, cb_h_ = height, cb_srgb_ = srgb, cb_factor_ = factor;
    }
    ID3D11ShaderResourceView* null_srv = nullptr;
    ctx->OMSetRenderTargets(1, &dst, nullptr);
    const D3D11_VIEWPORT vp{0, 0, float(width), float(height), 0, 1};
    ctx->RSSetViewports(1, &vp);
    ctx->RSSetState(raster_.Get());
    ctx->OMSetBlendState(blend_.Get(), nullptr, 0xFFFFFFFF);
    ctx->OMSetDepthStencilState(depth_.Get(), 0);
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(vs_.Get(), nullptr, 0);
    ctx->GSSetShader(nullptr, nullptr, 0);
    ctx->HSSetShader(nullptr, nullptr, 0);
    ctx->DSSetShader(nullptr, nullptr, 0);
    ctx->PSSetShader(down ? down_.Get() : ps_.Get(), nullptr, 0);
    ctx->PSSetConstantBuffers(0, 1, cb_.GetAddressOf());
    ctx->PSSetSamplers(0, 1, sampler_.GetAddressOf());
    ctx->PSSetShaderResources(0, 1, &src);
    // Test aid: MUA3_AA_TIME=1 logs the pass's GPU time (timestamp queries, read a few frames later).
    static const bool timed = std::getenv("MUA3_AA_TIME") != nullptr;
    Timer* t = nullptr;
    if (timed) {
        t = &timers_[timer_next_++ % std::size(timers_)];
        if (t->pending)
            ReadTimer(ctx, *t);
        if (!t->disjoint) {
            D3D11_QUERY_DESC qd{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
            dev->CreateQuery(&qd, &t->disjoint);
            qd.Query = D3D11_QUERY_TIMESTAMP;
            dev->CreateQuery(&qd, &t->begin);
            dev->CreateQuery(&qd, &t->end);
        }
        ctx->Begin(t->disjoint.Get());
        ctx->End(t->begin.Get());
    }
    ctx->Draw(3, 0);
    if (t) {
        ctx->End(t->end.Get());
        ctx->End(t->disjoint.Get());
        t->pending = true;
        t->name = down ? "SSAA resolve" : "FXAA";
    }
    ctx->PSSetShaderResources(0, 1, &null_srv);
    ID3D11RenderTargetView* null_rtv = nullptr;
    ctx->OMSetRenderTargets(1, &null_rtv, nullptr);
    return true;
}

} // namespace gpu
