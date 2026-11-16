// mua3_dlss.dll: DLSS Super Resolution through NGX's D3D11 API, behind the C interface in mua3_dlss_api.h.
// Built only with NVIDIA's SDK in third_party/dlss (proprietary, never committed). DLSS is a trademark of NVIDIA.
#include "dlss/mua3_dlss_api.h"

#include <cstdio>
#include <initializer_list>

#include <nvsdk_ngx.h>
#include <nvsdk_ngx_helpers_d3d.h>

namespace {

// This program's NGX project id (any GUID for NVSDK_NGX_ENGINE_TYPE_CUSTOM).
constexpr const char kProjectId[] = "6f2d4a8e-3c1b-4e7a-9d5f-2b8c1e4a7d30";

ID3D11Device* g_dev = nullptr;
NVSDK_NGX_Parameter* g_params = nullptr;
NVSDK_NGX_Handle* g_feature = nullptr;
unsigned g_in_w, g_in_h, g_out_w, g_out_h;
int g_quality = -1, g_inverted = -1;
unsigned g_preset = 0;

void Say(char* status, int size, const char* text, unsigned code = 0) {
    if (status && size > 0)
        std::snprintf(status, size_t(size), code ? "%s (%#x)" : "%s", text, code);
}

NVSDK_NGX_PerfQuality_Value PerfQuality(int quality) {
    switch (quality) {
    case 0: return NVSDK_NGX_PerfQuality_Value_UltraQuality;
    case 1: return NVSDK_NGX_PerfQuality_Value_MaxQuality;
    case 2: return NVSDK_NGX_PerfQuality_Value_Balanced;
    default: return NVSDK_NGX_PerfQuality_Value_MaxPerf;
    }
}

void ReleaseFeature() {
    if (g_feature)
        NVSDK_NGX_D3D11_ReleaseFeature(g_feature);
    g_feature = nullptr;
}

} // namespace

extern "C" {

__declspec(dllexport) int __cdecl mua3_dlss_api_version() { return MUA3_DLSS_API_VERSION; }

__declspec(dllexport) void __cdecl mua3_dlss_release() {
    ReleaseFeature();
    if (g_params)
        NVSDK_NGX_D3D11_DestroyParameters(g_params);
    g_params = nullptr;
    if (g_dev)
        NVSDK_NGX_D3D11_Shutdown1(g_dev);
    g_dev = nullptr;
    g_quality = g_inverted = -1;
}

__declspec(dllexport) int __cdecl mua3_dlss_init(ID3D11Device* dev, char* status, int status_size) {
    mua3_dlss_release();
    NVSDK_NGX_Result r = NVSDK_NGX_D3D11_Init_with_ProjectID(kProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM, "1.0", L".", dev);
    if (NVSDK_NGX_FAILED(r)) {
        Say(status, status_size, "NGX did not start (not an NVIDIA GPU, or an old driver)", unsigned(r));
        return 0;
    }
    g_dev = dev;
    if (NVSDK_NGX_FAILED(NVSDK_NGX_D3D11_GetCapabilityParameters(&g_params)) || !g_params) {
        Say(status, status_size, "NGX gave no capability parameters");
        mua3_dlss_release();
        return 0;
    }
    int available = 0, needs_driver = 0, init = 0;
    g_params->Get(NVSDK_NGX_Parameter_SuperSampling_Available, &available);
    g_params->Get(NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &needs_driver);
    if (!available) {
        g_params->Get(NVSDK_NGX_Parameter_SuperSampling_FeatureInitResult, &init);
        if (needs_driver)
            Say(status, status_size, "the NVIDIA driver is too old for this DLSS");
        else
            Say(status, status_size, "DLSS Super Resolution unavailable (nvngx_dlss.dll missing?)", unsigned(init));
        mua3_dlss_release();
        return 0;
    }
    Say(status, status_size, "ready");
    return 1;
}

__declspec(dllexport) int __cdecl mua3_dlss_evaluate(ID3D11DeviceContext* ctx, const Mua3DlssFrame* f, char* status,
                                                     int status_size) {
    if (!g_params || !f || !f->color || !f->depth || !f->motion || !f->output || !f->in_w || !f->in_h) {
        Say(status, status_size, "not initialised, or a frame without its inputs");
        return 0;
    }
    const unsigned preset = f->preset ? f->preset : unsigned(NVSDK_NGX_DLSS_Hint_Render_Preset_K);
    if (!g_feature || f->in_w != g_in_w || f->in_h != g_in_h || f->out_w != g_out_w || f->out_h != g_out_h ||
        f->quality != g_quality || f->depth_inverted != g_inverted || preset != g_preset) {
        ReleaseFeature();
        for (const char* hint : {NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality,
                                 NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance,
                                 NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance,
                                 NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraQuality})
            g_params->Set(hint, preset);
        NVSDK_NGX_DLSS_Create_Params cp{};
        cp.Feature.InWidth = f->in_w;
        cp.Feature.InHeight = f->in_h;
        cp.Feature.InTargetWidth = f->out_w;
        cp.Feature.InTargetHeight = f->out_h;
        cp.Feature.InPerfQualityValue = PerfQuality(f->quality);
        // The scene arrives tonemapped (LDR); motion vectors at the render size, not jittered.
        cp.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes |
                                  (f->depth_inverted ? NVSDK_NGX_DLSS_Feature_Flags_DepthInverted : 0);
        const NVSDK_NGX_Result r = NGX_D3D11_CREATE_DLSS_EXT(ctx, &g_feature, g_params, &cp);
        if (NVSDK_NGX_FAILED(r) || !g_feature) {
            g_feature = nullptr;
            Say(status, status_size, "creating DLSS failed", unsigned(r));
            return 0;
        }
        g_in_w = f->in_w, g_in_h = f->in_h, g_out_w = f->out_w, g_out_h = f->out_h;
        g_quality = f->quality, g_inverted = f->depth_inverted, g_preset = preset;
        if (status && status_size > 0)
            std::snprintf(status, size_t(status_size), "created %ux%u -> %ux%u, preset %c", f->in_w, f->in_h, f->out_w, f->out_h,
                          char('A' + preset - 1));
    }
    NVSDK_NGX_D3D11_DLSS_Eval_Params ep{};
    ep.Feature.pInColor = f->color;
    ep.Feature.pInOutput = f->output;
    ep.pInDepth = f->depth;
    ep.pInMotionVectors = f->motion;
    ep.InJitterOffsetX = f->jitter_x;
    ep.InJitterOffsetY = f->jitter_y;
    ep.InRenderSubrectDimensions = {f->in_w, f->in_h};
    ep.InReset = f->reset;
    ep.InMVScaleX = ep.InMVScaleY = 1.0f;
    const NVSDK_NGX_Result r = NGX_D3D11_EVALUATE_DLSS_EXT(ctx, g_feature, g_params, &ep);
    if (NVSDK_NGX_FAILED(r)) {
        Say(status, status_size, "evaluate failed", unsigned(r));
        return 0;
    }
    return 1;
}

} // extern "C"
