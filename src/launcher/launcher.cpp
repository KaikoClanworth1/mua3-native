// MUA3 Launcher: install from dumps, configure, and start the game.
#include <fstream>
#include <thread>

#include <d3d11.h>
#include <shellapi.h>
#include <wincodec.h>
#include <windows.h>

#include <SDL3/SDL.h>
#include <imgui.h>

#include "core/config.h"
#include "loader/installer.h"
#include "platform/platform.h"

#pragma comment(lib, "windowscodecs.lib")

namespace {

constexpr const char* kIni = "mua3.ini";
constexpr const char* kLauncherIni = "launcher.ini"; // launcher-only preferences (the game never reads them)

// UI scale: the monitor's content scale (150% / 200% on 4K screens) or the user's override. Every size in pixels
// below goes through S() so the launcher stays readable on high-DPI displays.
float g_ui = 1.0f;
float S(float px) { return px * g_ui; }
ImVec2 S(float x, float y) { return ImVec2(x * g_ui, y * g_ui); }

struct Texture {
    ID3D11ShaderResourceView* srv = nullptr;
    int w = 0, h = 0;
};

// Decodes an image (the game's JPEG icon) with WIC into a D3D11 texture.
Texture LoadImageTexture(ID3D11Device* dev, const fs::path& path) {
    Texture t;
    IWICImagingFactory* wic = nullptr;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))))
        return t;
    IWICBitmapDecoder* dec = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* conv = nullptr;
    if (SUCCEEDED(wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad,
                                                 &dec)) &&
        SUCCEEDED(dec->GetFrame(0, &frame)) && SUCCEEDED(wic->CreateFormatConverter(&conv)) &&
        SUCCEEDED(conv->Initialize(frame, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0,
                                   WICBitmapPaletteTypeCustom))) {
        UINT w = 0, h = 0;
        conv->GetSize(&w, &h);
        std::vector<u8> px(size_t(w) * h * 4);
        conv->CopyPixels(nullptr, w * 4, UINT(px.size()), px.data());
        D3D11_TEXTURE2D_DESC td{};
        td.Width = w;
        td.Height = h;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA sd{px.data(), w * 4, 0};
        ID3D11Texture2D* tex = nullptr;
        if (SUCCEEDED(dev->CreateTexture2D(&td, &sd, &tex))) {
            dev->CreateShaderResourceView(tex, nullptr, &t.srv);
            tex->Release();
            t.w = int(w);
            t.h = int(h);
        }
    }
    if (conv) conv->Release();
    if (frame) frame->Release();
    if (dec) dec->Release();
    wic->Release();
    return t;
}

void ApplyTheme() {
    ImGuiStyle& s = ImGui::GetStyle();
    s = ImGuiStyle();
    s.WindowRounding = 0;
    s.FrameRounding = 6;
    s.GrabRounding = 6;
    s.TabRounding = 6;
    s.ChildRounding = 8;
    s.FramePadding = ImVec2(10, 6);
    s.ItemSpacing = ImVec2(10, 8);
    ImVec4* c = s.Colors;
    const ImVec4 red(0.78f, 0.09f, 0.13f, 1.0f), red_hi(0.90f, 0.16f, 0.20f, 1.0f), red_lo(0.55f, 0.06f, 0.09f, 1.0f);
    c[ImGuiCol_WindowBg] = ImVec4(0.07f, 0.07f, 0.09f, 1.0f);
    c[ImGuiCol_ChildBg] = ImVec4(0.11f, 0.11f, 0.14f, 1.0f);
    c[ImGuiCol_FrameBg] = ImVec4(0.16f, 0.16f, 0.20f, 1.0f);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.22f, 0.22f, 0.27f, 1.0f);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.26f, 0.26f, 0.32f, 1.0f);
    c[ImGuiCol_Button] = red_lo;
    c[ImGuiCol_ButtonHovered] = red;
    c[ImGuiCol_ButtonActive] = red_hi;
    c[ImGuiCol_Header] = red_lo;
    c[ImGuiCol_HeaderHovered] = red;
    c[ImGuiCol_HeaderActive] = red_hi;
    c[ImGuiCol_Tab] = ImVec4(0.14f, 0.14f, 0.18f, 1.0f);
    c[ImGuiCol_TabHovered] = red;
    c[ImGuiCol_TabSelected] = red_lo;
    c[ImGuiCol_CheckMark] = red_hi;
    c[ImGuiCol_SliderGrab] = red;
    c[ImGuiCol_SliderGrabActive] = red_hi;
    c[ImGuiCol_PlotHistogram] = red;
    c[ImGuiCol_Separator] = ImVec4(0.25f, 0.25f, 0.30f, 1.0f);
    s.ScaleAllSizes(g_ui);
}

// Fonts are rasterised at the UI scale (sharp text on 4K instead of a stretched 19 px font).
void LoadFonts() {
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    if (fs::exists("C:\\Windows\\Fonts\\segoeui.ttf"))
        io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", S(19.0f));
    else {
        ImFontConfig fc;
        fc.SizePixels = S(13.0f);
        io.Fonts->AddFontDefault(&fc);
    }
}

bool PathInput(const char* label, fs::path& p) {
    char buf[512];
    std::snprintf(buf, sizeof(buf), "%s", p.string().c_str());
    if (ImGui::InputText(label, buf, sizeof(buf))) {
        p = fs::u8path(buf);
        return true;
    }
    return false;
}

void Help(const char* text) {
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::BeginItemTooltip()) {
        ImGui::PushTextWrapPos(S(420));
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

void OpenFolder(const fs::path& p) {
    std::error_code ec;
    fs::create_directories(p, ec);
    ShellExecuteW(nullptr, L"open", fs::absolute(p).wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

bool LaunchGame(bool console, std::string& err) {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    fs::path dir = fs::path(exe).parent_path();
    fs::path game = dir / "mua3.exe";
    if (!fs::exists(game)) {
        err = "mua3.exe not found next to the launcher";
        return false;
    }
    std::wstring cmd = L"\"" + game.wstring() + L"\"";
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    const DWORD flags = console ? CREATE_NEW_CONSOLE : CREATE_NO_WINDOW;
    wchar_t cwd[MAX_PATH];
    GetCurrentDirectoryW(MAX_PATH, cwd);
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, flags, nullptr, cwd, &si, &pi)) {
        err = fmt::format("CreateProcess failed ({})", GetLastError());
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

// A connected monitor, as SDL reports it.
struct Monitor {
    std::string name;
    int w = 0, h = 0;
    float refresh = 0, scale = 1;
};

std::vector<Monitor> ListMonitors() {
    std::vector<Monitor> out;
    int n = 0;
    SDL_DisplayID* ids = SDL_GetDisplays(&n);
    for (int i = 0; i < n && ids; i++) {
        Monitor m;
        const char* name = SDL_GetDisplayName(ids[i]);
        m.name = name ? name : fmt::format("Display {}", i + 1);
        if (const SDL_DisplayMode* d = SDL_GetDesktopDisplayMode(ids[i])) {
            m.w = d->w;
            m.h = d->h;
            m.refresh = d->refresh_rate;
        }
        m.scale = SDL_GetDisplayContentScale(ids[i]);
        out.push_back(m);
    }
    SDL_free(ids);
    return out;
}

// Game frame rates the engine patch can encode (the float must have zero low 16 bits).
constexpr int kRates[] = {30, 60, 72, 90, 120, 144, 165, 240};

// Highest supported rate that divides the monitor's refresh evenly and stays playable (<= 72 unless the user
// opts in): 60 on 60/120 Hz, 72 on 144 Hz, ...
int RecommendedRate(float refresh) {
    const int hz = int(refresh + 0.5f);
    for (int r : {72, 60})
        if (hz > 0 && hz % r == 0)
            return r;
    return 60;
}

struct LauncherState {
    core::Config cfg;
    loader::DumpSummary dumps;
    bool keys_ok = false, title_keys = false, installed = false;
    loader::InstallProgress progress;
    std::thread worker;
    std::atomic<bool> installing{false};
    std::string install_result;
    std::string status;
    Texture icon;
    std::vector<Monitor> monitors;
    // launcher.ini
    bool show_console = false;
    bool close_on_play = true;
    int ui_scale_pct = 0; // 0 = follow the monitor
    int muted_volume = -1; // volume to restore when unmuting

    void Refresh() {
        dumps = loader::ScanDumps(cfg.isos_dir);
        keys_ok = fs::exists(cfg.keys_dir / "prod.keys");
        title_keys = fs::exists(cfg.keys_dir / "title.keys");
        installed = loader::IsInstalled(cfg.game_dir);
    }
    void LoadPrefs() {
        std::ifstream f(kLauncherIni);
        for (std::string line; std::getline(f, line);) {
            const auto eq = line.find('=');
            if (eq == std::string::npos)
                continue;
            const std::string k = line.substr(0, eq);
            const int v = std::atoi(line.c_str() + eq + 1);
            if (k == "show_console") show_console = v != 0;
            else if (k == "close_on_play") close_on_play = v != 0;
            else if (k == "ui_scale") ui_scale_pct = v;
            else if (k == "muted_volume") muted_volume = v;
        }
    }
    void SavePrefs() const {
        std::ofstream f(kLauncherIni);
        f << "show_console=" << show_console << "\nclose_on_play=" << close_on_play << "\nui_scale=" << ui_scale_pct
          << "\nmuted_volume=" << muted_volume << "\n";
    }
    const Monitor* Primary() const { return monitors.empty() ? nullptr : &monitors[0]; }
};

void Header(LauncherState& st) {
    if (st.icon.srv) {
        ImGui::Image((ImTextureID)st.icon.srv, S(112, 112));
        ImGui::SameLine();
    }
    ImGui::BeginGroup();
    ImGui::SetWindowFontScale(1.6f);
    ImGui::TextUnformatted("MARVEL ULTIMATE ALLIANCE 3");
    ImGui::SetWindowFontScale(1.1f);
    ImGui::TextColored(ImVec4(0.9f, 0.2f, 0.25f, 1), "The Black Order");
    ImGui::SetWindowFontScale(1.0f);
    ImGui::TextDisabled("Native Windows build  |  D3D11  |  XAudio2/SDL  |  XInput/SDL");
    ImGui::Text("Game data: %s", st.installed ? "installed" : "not installed");
    ImGui::EndGroup();
    ImGui::Separator();
}

// Internal render size the game will use for these settings (mirrors main.cpp's auto scale).
std::pair<int, int> RenderSize(const core::Config& c) {
    const int base_h = c.docked ? 1080 : 720, base_w = c.docked ? 1920 : 1280;
    int scale = c.resolution_scale;
    if (scale <= 0)
        scale = (c.height + base_h - 1) / base_h;
    scale = std::clamp(scale, 1, 4);
    return {base_w * scale, base_h * scale};
}

void PlayTab(LauncherState& st) {
    ImGui::Spacing();
    if (!st.installed) {
        ImGui::TextWrapped("The game data hasn't been installed yet. Go to the Install tab to decrypt your dumps.");
        return;
    }
    const auto& c = st.cfg;
    ImGui::BeginChild("play", ImVec2(0, S(170)), ImGuiChildFlags_Borders);
    const auto [rw, rh] = RenderSize(c);
    ImGui::Text("Display: %d x %d %s   |   renders at %d x %d", c.width, c.height, c.fullscreen ? "fullscreen" : "window",
                rw, rh);
    ImGui::Text("Frame rate: %d fps%s   |   %s mode", c.fps_target, c.fps_target > 60 ? " (experimental)" : "",
                c.docked ? "Docked" : "Handheld");
    const char* audio[] = {"XAudio2", "SDL", "None"};
    const char* input[] = {"XInput", "SDL"};
    ImGui::Text("Audio: %s, %d%%   |   Input: %s", audio[int(c.audio)], c.volume, input[int(c.input)]);
    if (ImGui::Checkbox("Show log console", &st.show_console))
        st.SavePrefs();
    ImGui::SameLine();
    if (ImGui::Checkbox("Close launcher on play", &st.close_on_play))
        st.SavePrefs();
    ImGui::EndChild();
    ImGui::Spacing();
    if (ImGui::Button("  PLAY  ", S(220, 56))) {
        st.cfg.Save(kIni);
        std::string err;
        if (LaunchGame(st.show_console, err)) {
            st.status = "Game started.";
            if (st.close_on_play)
                PostQuitMessage(0), std::exit(0);
        } else {
            st.status = err;
        }
    }
}

void InstallTab(LauncherState& st) {
    ImGui::Spacing();
    bool changed = false;
    changed |= PathInput("Dumps folder (NSP/XCI)", st.cfg.isos_dir);
    changed |= PathInput("Keys folder", st.cfg.keys_dir);
    changed |= PathInput("Install folder", st.cfg.game_dir);
    if (changed)
        st.Refresh();
    ImGui::Spacing();
    auto check = [](bool ok, const char* text) {
        ImGui::TextColored(ok ? ImVec4(0.3f, 0.85f, 0.4f, 1) : ImVec4(0.95f, 0.35f, 0.3f, 1), ok ? "  OK  " : "  --  ");
        ImGui::SameLine();
        ImGui::TextUnformatted(text);
    };
    check(st.keys_ok, "prod.keys (from your own console, Lockpick_RCM)");
    check(st.dumps.has_base, "Base game dump");
    check(st.dumps.has_update, "Update (v3.0.0 recommended)");
    check(st.dumps.dlc_count > 0, fmt::format("DLC ({} found)", st.dumps.dlc_count).c_str());
    ImGui::BeginChild("dumps", ImVec2(0, S(110)), ImGuiChildFlags_Borders);
    for (auto& l : st.dumps.lines)
        ImGui::TextUnformatted(l.c_str());
    ImGui::EndChild();

    if (st.installing) {
        const u64 t = st.progress.bytes_total, d = st.progress.bytes_done;
        const float f = t ? float(double(d) / double(t)) : 0.0f;
        ImGui::Text("%s", st.progress.Stage().c_str());
        ImGui::ProgressBar(f, ImVec2(-1, 0), fmt::format("{:.2f} / {:.2f} GB", d / 1073741824.0, t / 1073741824.0).c_str());
        if (ImGui::Button("Cancel"))
            st.progress.cancel = true;
    } else {
        ImGui::BeginDisabled(!st.keys_ok || (!st.dumps.has_base && !st.dumps.has_update));
        if (ImGui::Button(st.installed ? "Reinstall game data" : "Install game data", S(260, 40))) {
            st.cfg.Save(kIni);
            st.installing = true;
            st.install_result.clear();
            st.progress.cancel = false;
            st.progress.bytes_done = 0;
            st.progress.bytes_total = 0;
            if (st.worker.joinable())
                st.worker.join();
            st.worker = std::thread([&st] {
                loader::InstallOptions o{st.cfg.isos_dir, st.cfg.keys_dir, st.cfg.game_dir, true};
                std::string err;
                const bool ok = loader::InstallGame(o, st.progress, err);
                st.install_result = ok ? "Install complete." : "Install failed: " + err;
                st.installing = false;
                st.Refresh();
            });
        }
        ImGui::EndDisabled();
        if (!st.install_result.empty())
            ImGui::TextWrapped("%s", st.install_result.c_str());
    }
}

void DisplaySettings(LauncherState& st) {
    auto& c = st.cfg;
    const Monitor* mon = st.Primary();
    if (mon)
        ImGui::TextDisabled("Main display: %s, %d x %d @ %.0f Hz, %.0f%% scale", mon->name.c_str(), mon->w, mon->h,
                            mon->refresh, mon->scale * 100);

    int mode = c.fullscreen ? 1 : 0;
    const char* modes[] = {"Windowed", "Fullscreen (borderless)"};
    if (ImGui::Combo("Display mode", &mode, modes, 2))
        c.fullscreen = mode == 1;
    Help("Fullscreen covers the monitor at its desktop resolution. F11 toggles it in game.");

    // Window size presets, the monitor's own resolution, or a custom size.
    struct Preset {
        const char* name;
        int w, h;
    };
    std::vector<Preset> presets = {{"1280 x 720 (720p)", 1280, 720},    {"1600 x 900", 1600, 900},
                                   {"1920 x 1080 (1080p)", 1920, 1080},  {"2560 x 1440 (1440p)", 2560, 1440},
                                   {"3200 x 1800", 3200, 1800},          {"3840 x 2160 (4K)", 3840, 2160},
                                   {"2560 x 1080 (ultrawide)", 2560, 1080}, {"3440 x 1440 (ultrawide)", 3440, 1440},
                                   {"5120 x 2880 (5K)", 5120, 2880}};
    std::string native = mon ? fmt::format("Match display ({} x {})", mon->w, mon->h) : "Match display";
    int cur = -1;
    if (mon && c.width == mon->w && c.height == mon->h)
        cur = 0;
    else
        for (size_t i = 0; i < presets.size(); i++)
            if (presets[i].w == c.width && presets[i].h == c.height)
                cur = int(i) + 1;
    const int custom = int(presets.size()) + 1;
    const char* preview = cur == 0 ? native.c_str() : cur > 0 ? presets[cur - 1].name : "Custom";
    static bool force_custom = false;
    if (force_custom)
        preview = "Custom";
    if (ImGui::BeginCombo("Resolution", preview)) {
        if (mon && ImGui::Selectable(native.c_str(), cur == 0 && !force_custom))
            c.width = mon->w, c.height = mon->h, force_custom = false;
        for (size_t i = 0; i < presets.size(); i++)
            if (ImGui::Selectable(presets[i].name, cur == int(i) + 1 && !force_custom))
                c.width = presets[i].w, c.height = presets[i].h, force_custom = false;
        if (ImGui::Selectable("Custom", cur < 0 || force_custom))
            force_custom = true;
        ImGui::EndCombo();
    }
    if (cur < 0 || force_custom) {
        ImGui::SetNextItemWidth(S(120));
        ImGui::InputInt("Width", &c.width, 0);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(S(120));
        ImGui::InputInt("Height", &c.height, 0);
        c.width = std::clamp(c.width, 640, 7680);
        c.height = std::clamp(c.height, 360, 4320);
    }
    (void)custom;
    Help("Window size in windowed mode. In fullscreen the game uses the monitor's resolution.");

    int scale = c.resolution_scale;
    const auto [rw, rh] = RenderSize(c);
    const std::string label = scale <= 0 ? fmt::format("Auto ({} x {})", rw, rh) : fmt::format("{}x ({} x {})", scale, rw, rh);
    if (ImGui::SliderInt("Internal resolution", &scale, 0, 4, label.c_str()))
        c.resolution_scale = scale;
    Help("The game draws at 1080p (docked). Auto renders at the smallest multiple that covers your display, so "
         "1440p and 4K screens get a sharp image instead of an upscaled 1080p one. Higher is sharper and slower.");
}

void PerformanceSettings(LauncherState& st) {
    auto& c = st.cfg;
    const Monitor* mon = st.Primary();
    int cur = 1;
    for (int i = 0; i < int(std::size(kRates)); i++)
        if (c.fps_target == kRates[i])
            cur = i;
    std::vector<std::string> names;
    for (int r : kRates)
        names.push_back(r == 30 ? "30 (original)" : r <= 60 ? std::to_string(r) : fmt::format("{} (experimental)", r));
    if (ImGui::BeginCombo("Game frame rate", names[cur].c_str())) {
        for (int i = 0; i < int(std::size(kRates)); i++)
            if (ImGui::Selectable(names[i].c_str(), i == cur))
                c.fps_target = kRates[i];
        ImGui::EndCombo();
    }
    Help("The game's engine supports 60 fps natively. Rates above 60 change its timing and may run faster in places. "
         "For smooth motion pick a rate that divides your monitor's refresh rate (60 on 60/120 Hz, 72 on 144 Hz).");
    if (mon) {
        const int rec = RecommendedRate(mon->refresh);
        ImGui::SameLine();
        if (c.fps_target != rec && ImGui::SmallButton(fmt::format("Use {} for {:.0f} Hz", rec, mon->refresh).c_str()))
            c.fps_target = rec;
    }
    ImGui::SliderInt("FPS limit", &c.fps_limit, 0, 240, c.fps_limit <= 0 ? "Off" : "%d");
    Help("Caps how fast frames are presented when the game is not limiting itself. 0 = off.");
    ImGui::Checkbox("VSync", &c.vsync);
    ImGui::SameLine();
    ImGui::Checkbox("Show FPS", &c.show_fps);
    ImGui::SameLine();
    ImGui::Checkbox("Debug overlay (F1)", &c.show_debug_overlay);
    if (ImGui::TreeNode("Advanced")) {
        int si = c.swap_interval;
        const char* sis[] = {"Game decides", "Every frame (1)", "Every 2nd vblank (2)"};
        if (ImGui::Combo("Present interval", &si, sis, 3))
            c.swap_interval = si;
        Help("Overrides the interval the game asks for. Leave on 'Game decides' unless testing.");
        ImGui::TreePop();
    }
}

void SettingsTab(LauncherState& st) {
    auto& c = st.cfg;
    ImGui::Spacing();
    if (ImGui::CollapsingHeader("Display", ImGuiTreeNodeFlags_DefaultOpen))
        DisplaySettings(st);
    if (ImGui::CollapsingHeader("Performance", ImGuiTreeNodeFlags_DefaultOpen))
        PerformanceSettings(st);
    if (ImGui::CollapsingHeader("Audio", ImGuiTreeNodeFlags_DefaultOpen)) {
        int a = int(c.audio);
        const char* names[] = {"XAudio2", "SDL3", "Disabled"};
        if (ImGui::Combo("Audio backend", &a, names, 3))
            c.audio = core::AudioBackend(a);
        ImGui::SliderInt("Volume", &c.volume, 0, 100, "%d%%");
        bool muted = c.volume == 0 && st.muted_volume > 0;
        ImGui::SameLine();
        if (ImGui::Checkbox("Mute", &muted)) {
            if (muted)
                st.muted_volume = std::max(c.volume, 1), c.volume = 0;
            else
                c.volume = st.muted_volume > 0 ? st.muted_volume : 100, st.muted_volume = -1;
            st.SavePrefs();
        }
    }
    if (ImGui::CollapsingHeader("Input", ImGuiTreeNodeFlags_DefaultOpen)) {
        int i = int(c.input);
        const char* names[] = {"XInput (Xbox controllers)", "SDL3 (PlayStation, Switch Pro, generic)"};
        if (ImGui::Combo("Controller backend", &i, names, 2))
            c.input = core::InputBackend(i);
        ImGui::TextDisabled("Keyboard: WASD move, L/K/I/J = A/B/X/Y, Q/E = L/R, 1/3 = ZL/ZR, Enter = +, Tab = -");
    }
    if (ImGui::CollapsingHeader("System")) {
        ImGui::Checkbox("Docked mode (1080p)", &c.docked);
        Help("Docked renders at 1080p, handheld at 720p.");
        const char* langs[] = {"Japanese", "English (US)", "French", "German", "Italian", "Spanish", "Chinese",
                               "Korean", "Dutch", "Portuguese", "Russian", "Taiwanese", "English (UK)",
                               "French (Canada)", "Spanish (Latin America)", "Chinese (Simplified)",
                               "Chinese (Traditional)"};
        ImGui::Combo("Language", &c.language, langs, 17);
        const char* levels[] = {"Trace", "Debug", "Info", "Warning", "Error"};
        ImGui::Combo("Log level", &c.log_level, levels, 5);
        PathInput("Save folder", c.save_dir);
    }
    if (ImGui::CollapsingHeader("Launcher")) {
        const int pcts[] = {0, 100, 125, 150, 175, 200, 250};
        int cur = 0;
        for (int i = 0; i < 7; i++)
            if (st.ui_scale_pct == pcts[i])
                cur = i;
        const char* names[] = {"Auto (follow Windows)", "100%", "125%", "150%", "175%", "200%", "250%"};
        if (ImGui::Combo("Launcher size", &cur, names, 7)) {
            st.ui_scale_pct = pcts[cur];
            st.SavePrefs();
            st.status = "Launcher size changes on the next start.";
        }
    }
    ImGui::Spacing();
    if (ImGui::Button("Save settings", S(200, 36)))
        st.status = c.Save(kIni) ? "Settings saved." : "Could not write mua3.ini";
    ImGui::SameLine();
    if (ImGui::Button("Reset to defaults", S(200, 36))) {
        auto paths = std::make_tuple(c.game_dir, c.isos_dir, c.keys_dir, c.save_dir);
        c = core::Config{};
        std::tie(c.game_dir, c.isos_dir, c.keys_dir, c.save_dir) = paths;
        if (const Monitor* mon = st.Primary()) { // sensible defaults for this machine
            c.width = std::min(mon->w, 1920);
            c.height = std::min(mon->h, 1080);
        }
    }
}

void ToolsTab(LauncherState& st) {
    ImGui::Spacing();
    if (ImGui::Button("Open save folder", S(240, 36)))
        OpenFolder(st.cfg.save_dir);
    ImGui::SameLine();
    if (ImGui::Button("Open screenshots", S(240, 36)))
        OpenFolder("screenshots");
    ImGui::SameLine();
    if (ImGui::Button("Open game folder", S(240, 36)))
        OpenFolder(".");
    ImGui::Spacing();
    if (fs::exists("mua3.log") && ImGui::Button("Open last log", S(240, 36)))
        ShellExecuteW(nullptr, L"open", fs::absolute("mua3.log").wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextUnformatted("Shader cache");
    size_t files = 0;
    std::error_code ec;
    static size_t cached_files = size_t(-1);
    static double last_scan = -10;
    if (ImGui::GetTime() - last_scan > 5) { // directory scans are not free; refresh every few seconds
        cached_files = 0;
        for (auto it = fs::directory_iterator("shader_cache", ec); !ec && it != fs::directory_iterator(); it.increment(ec))
            cached_files++;
        last_scan = ImGui::GetTime();
    }
    files = cached_files;
    ImGui::TextDisabled("%zu compiled shaders. Clearing it makes the next start recompile them (brief pop-in).", files);
    static bool confirm = false;
    if (!confirm) {
        if (ImGui::Button("Clear shader cache", S(240, 36)))
            confirm = true;
    } else {
        ImGui::TextColored(ImVec4(0.95f, 0.6f, 0.3f, 1), "Delete all cached shaders?");
        ImGui::SameLine();
        if (ImGui::Button("Yes, clear")) {
            for (auto it = fs::directory_iterator("shader_cache", ec); !ec && it != fs::directory_iterator(); it.increment(ec))
                fs::remove(it->path(), ec);
            st.status = "Shader cache cleared.";
            last_scan = -10;
            confirm = false;
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel"))
            confirm = false;
    }
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextUnformatted("Displays");
    for (auto& m : st.monitors)
        ImGui::BulletText("%s: %d x %d @ %.0f Hz, %.0f%% scale", m.name.c_str(), m.w, m.h, m.refresh, m.scale * 100);
}

void AboutTab() {
    ImGui::Spacing();
    ImGui::TextWrapped(
        "A private native Windows port of Marvel Ultimate Alliance 3 built from your own dumps. The game code "
        "currently runs on an AArch64 JIT with high-level emulation of the Switch OS; functions are being replaced "
        "with native implementations step by step.");
    ImGui::Spacing();
    ImGui::BulletText("Renderer: Direct3D 11, up to 4x internal resolution");
    ImGui::BulletText("Audio: XAudio2 or SDL3");
    ImGui::BulletText("Movies: Windows Media Foundation");
    ImGui::BulletText("Input: XInput or SDL3 gamepads, keyboard");
    ImGui::BulletText("Third-party: dynarmic, SDL3, Dear ImGui, Boost (headers)");
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    // Run from the executable's folder unless a config already exists in the current one.
    if (!fs::exists(kIni)) {
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        const fs::path dir = fs::path(exe).parent_path();
        for (fs::path p = dir; !p.empty() && p != p.parent_path(); p = p.parent_path())
            if (fs::exists(p / "Isos") || fs::exists(p / "game") || fs::exists(p / kIni)) {
                fs::current_path(p);
                break;
            }
    }
    LauncherState st;
    st.cfg.Load(kIni);
    st.LoadPrefs();
    st.Refresh();

    core::Config wcfg;
    wcfg.width = 980;
    wcfg.height = 720;
    wcfg.vsync = true;
    platform::Display display;
    if (!display.Init(wcfg, "MUA3 Launcher"))
        return 1;
    st.monitors = ListMonitors();

    // Size everything for the monitor the window opened on (or the user's override), then grow the window to
    // match and keep it on screen.
    SDL_Window* win = display.Window();
    const float content = SDL_GetDisplayContentScale(SDL_GetDisplayForWindow(win));
    g_ui = st.ui_scale_pct > 0 ? st.ui_scale_pct / 100.0f : (content > 0 ? content : 1.0f);
    g_ui = std::clamp(g_ui, 1.0f, 3.0f);
    if (g_ui != 1.0f) {
        SDL_Rect usable{};
        SDL_GetDisplayUsableBounds(SDL_GetDisplayForWindow(win), &usable);
        int w = int(S(980)), h = int(S(720));
        if (usable.w > 0)
            w = std::min(w, usable.w - 40), h = std::min(h, usable.h - 60);
        SDL_SetWindowSize(win, w, h);
        SDL_SetWindowPosition(win, SDL_WINDOWPOS_CENTERED_DISPLAY(SDL_GetDisplayForWindow(win)),
                              SDL_WINDOWPOS_CENTERED_DISPLAY(SDL_GetDisplayForWindow(win)));
    }
    ApplyTheme();
    LoadFonts();
    st.icon = LoadImageTexture(display.Device(), st.cfg.game_dir / "control" / "icon_AmericanEnglish.dat");

    display.draw_overlay = [&] {
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->Pos);
        ImGui::SetNextWindowSize(vp->Size);
        ImGui::Begin("Launcher", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus);
        Header(st);
        if (ImGui::BeginTabBar("tabs")) {
            if (ImGui::BeginTabItem("Play")) {
                PlayTab(st);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Install")) {
                InstallTab(st);
                ImGui::EndTabItem();
            }
            static const std::string want_tab = std::getenv("MUA3_LAUNCHER_TAB") ? std::getenv("MUA3_LAUNCHER_TAB") : "";
            static int tab_frames = 0;
            auto flag = [&](const char* name) {
                return want_tab == name && tab_frames < 3 ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
            };
            tab_frames++;
            if (ImGui::BeginTabItem("Settings", nullptr, flag("Settings"))) {
                ImGui::BeginChild("settings", ImVec2(0, -S(40)));
                SettingsTab(st);
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Tools", nullptr, flag("Tools"))) {
                ToolsTab(st);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("About")) {
                AboutTab();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        if (!st.status.empty()) {
            ImGui::SetCursorPosY(ImGui::GetWindowHeight() - S(34));
            ImGui::TextDisabled("%s", st.status.c_str());
        }
        ImGui::End();
    };

    // Automation (MUA3_HEADLESS + MUA3_LAUNCHER_SHOT=<file.bmp>[, MUA3_LAUNCHER_TAB=<tab>]): save a frame, exit.
    const char* shot = std::getenv("MUA3_LAUNCHER_SHOT");
    for (int frame = 0; display.PumpEvents(); frame++) {
        if (shot && frame == 20)
            display.capture_path = fs::u8path(shot);
        if (shot && frame == 22)
            break;
        display.BeginFrame();
        display.EndFrame();
        display.PaceFrame();
    }
    st.progress.cancel = true;
    if (st.worker.joinable())
        st.worker.join();
    display.Shutdown();
    return 0;
}
