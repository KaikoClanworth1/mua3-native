#include "gpu/d3d11/dlss.h"

#include <cstdlib>
#include <windows.h>

#include "dlss/mua3_dlss_api.h"

namespace gpu {

namespace {

struct Module {
    HMODULE dll = nullptr;
    Mua3DlssInitFn init = nullptr;
    Mua3DlssEvaluateFn evaluate = nullptr;
    Mua3DlssReleaseFn release = nullptr;
};

// mua3_dlss.dll next to mua3.exe (only builds made with NVIDIA's SDK have it).
Module& Dll(std::string& why) {
    static Module m;
    static bool tried = false;
    static std::string reason;
    if (!tried) {
        tried = true;
        wchar_t path[MAX_PATH];
        const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
        std::wstring dir(path, n);
        dir.resize(dir.find_last_of(L"\\/") + 1);
        m.dll = LoadLibraryExW((dir + L"mua3_dlss.dll").c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!m.dll)
            reason = "mua3_dlss.dll is not installed (a build without NVIDIA's DLSS SDK)";
        else {
            auto version = reinterpret_cast<Mua3DlssApiVersionFn>(GetProcAddress(m.dll, "mua3_dlss_api_version"));
            m.init = reinterpret_cast<Mua3DlssInitFn>(GetProcAddress(m.dll, "mua3_dlss_init"));
            m.evaluate = reinterpret_cast<Mua3DlssEvaluateFn>(GetProcAddress(m.dll, "mua3_dlss_evaluate"));
            m.release = reinterpret_cast<Mua3DlssReleaseFn>(GetProcAddress(m.dll, "mua3_dlss_release"));
            if (!version || version() != MUA3_DLSS_API_VERSION || !m.init || !m.evaluate || !m.release) {
                reason = "mua3_dlss.dll is from another version of the game";
                m = Module{};
            }
        }
    }
    why = reason;
    return m;
}

} // namespace

Dlss::~Dlss() { Shutdown(); }

bool Dlss::Available(ID3D11Device* dev) {
    if (tried_)
        return ok_;
    tried_ = true;
    Module& m = Dll(status_);
    if (!m.init) {
        LOG_INFO("Renderer", "DLSS: {}", status_);
        return false;
    }
    char why[256] = {};
    if (!m.init(dev, why, sizeof(why))) {
        status_ = why;
        LOG_INFO("Renderer", "DLSS: {}", status_);
        return false;
    }
    dev_ = dev;
    status_ = "ready";
    LOG_INFO("Renderer", "DLSS Super Resolution available (mua3_dlss.dll, NGX D3D11)");
    return ok_ = true;
}

bool Dlss::Evaluate(ID3D11Device* dev, ID3D11DeviceContext* ctx, const Frame& f) {
    if (!Available(dev))
        return false;
    std::string unused;
    Module& m = Dll(unused);
    Mua3DlssFrame mf{};
    mf.color = f.color, mf.depth = f.depth, mf.motion = f.motion, mf.output = f.output;
    mf.in_w = f.in_w, mf.in_h = f.in_h, mf.out_w = f.out_w, mf.out_h = f.out_h;
    mf.jitter_x = f.jitter_x, mf.jitter_y = f.jitter_y;
    mf.reset = f.reset ? 1 : 0;
    mf.quality = f.quality;
    // MUA3_DLSS_PRESET=<letter> picks another render preset than K (the transformer model).
    if (const char* e = std::getenv("MUA3_DLSS_PRESET"); e && e[0] >= 'A' && e[0] <= 'O')
        mf.preset = unsigned(e[0] - 'A' + 1);
    mf.depth_inverted = std::getenv("MUA3_DLSS_DEPTH_INVERTED") ? 1 : 0;
    char why[256] = {};
    const bool ok = m.evaluate(ctx, &mf, why, sizeof(why)) != 0;
    if (why[0] && (!ok || std::string_view(why).starts_with("created"))) {
        static int logged = 0;
        if (ok || logged < 4) {
            logged += !ok;
            LOG_INFO("Renderer", "DLSS: {}", why);
        }
    }
    return ok;
}

void Dlss::Shutdown() {
    std::string unused;
    if (ok_)
        if (Module& m = Dll(unused); m.release)
            m.release();
    dev_ = nullptr; // (tried_ stays: an unavailable DLSS is not retried every frame; Reset() starts over)
    ok_ = false;
}

void Dlss::Reset() {
    Shutdown();
    tried_ = false;
    status_ = "not initialised";
}

} // namespace gpu
