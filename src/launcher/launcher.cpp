// MUA3 Launcher: install from dumps, configure, and start the game.
#include <thread>

#include <d3d11.h>
#include <wincodec.h>
#include <windows.h>

#include <imgui.h>

#include "core/config.h"
#include "loader/installer.h"
#include "platform/platform.h"

#pragma comment(lib, "windowscodecs.lib")

namespace {

constexpr const char* kIni = "mua3.ini";

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

struct LauncherState {
    core::Config cfg;
    loader::DumpSummary dumps;
    bool keys_ok = false, title_keys = false, installed = false;
    loader::InstallProgress progress;
    std::thread worker;
    std::atomic<bool> installing{false};
    std::string install_result;
    std::string status;
    bool show_console = false;
    bool close_on_play = true;
    Texture icon;
    std::string version = "3.0.0";

    void Refresh() {
        dumps = loader::ScanDumps(cfg.isos_dir);
        keys_ok = fs::exists(cfg.keys_dir / "prod.keys");
        title_keys = fs::exists(cfg.keys_dir / "title.keys");
        installed = loader::IsInstalled(cfg.game_dir);
    }
};

void Header(LauncherState& st) {
    if (st.icon.srv) {
        ImGui::Image((ImTextureID)st.icon.srv, ImVec2(112, 112));
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

void PlayTab(LauncherState& st) {
    ImGui::Spacing();
    if (!st.installed) {
        ImGui::TextWrapped("The game data hasn't been installed yet. Go to the Install tab to decrypt your dumps.");
        return;
    }
    ImGui::BeginChild("play", ImVec2(0, 150), ImGuiChildFlags_Borders);
    ImGui::Text("Resolution: %d x %d %s", st.cfg.width, st.cfg.height, st.cfg.fullscreen ? "(fullscreen)" : "(windowed)");
    ImGui::Text("Mode: %s   VSync: %s   FPS limit: %d", st.cfg.docked ? "Docked" : "Handheld",
                st.cfg.vsync ? "on" : "off", st.cfg.fps_limit);
    const char* audio[] = {"XAudio2", "SDL", "None"};
    const char* input[] = {"XInput", "SDL"};
    ImGui::Text("Audio: %s   Input: %s", audio[int(st.cfg.audio)], input[int(st.cfg.input)]);
    ImGui::Checkbox("Show log console", &st.show_console);
    ImGui::SameLine();
    ImGui::Checkbox("Close launcher on play", &st.close_on_play);
    ImGui::EndChild();
    ImGui::Spacing();
    if (ImGui::Button("  PLAY  ", ImVec2(220, 56))) {
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
    ImGui::BeginChild("dumps", ImVec2(0, 110), ImGuiChildFlags_Borders);
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
        if (ImGui::Button(st.installed ? "Reinstall game data" : "Install game data", ImVec2(260, 40))) {
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

void SettingsTab(LauncherState& st) {
    auto& c = st.cfg;
    ImGui::Spacing();
    if (ImGui::CollapsingHeader("Video", ImGuiTreeNodeFlags_DefaultOpen)) {
        static const int res[][2] = {{1280, 720}, {1600, 900}, {1920, 1080}, {2560, 1440}, {3840, 2160}};
        int cur = 2;
        for (int i = 0; i < 5; i++)
            if (res[i][0] == c.width && res[i][1] == c.height)
                cur = i;
        const char* names[] = {"1280 x 720", "1600 x 900", "1920 x 1080", "2560 x 1440", "3840 x 2160"};
        if (ImGui::Combo("Window size", &cur, names, 5)) {
            c.width = res[cur][0];
            c.height = res[cur][1];
        }
        ImGui::Checkbox("Fullscreen", &c.fullscreen);
        ImGui::SameLine();
        ImGui::Checkbox("VSync", &c.vsync);
        ImGui::SliderInt("Internal resolution scale", &c.resolution_scale, 1, 4, "%dx");
        ImGui::SliderInt("FPS limit (0 = off)", &c.fps_limit, 0, 240);
        ImGui::Checkbox("Show FPS", &c.show_fps);
        ImGui::SameLine();
        ImGui::Checkbox("Debug overlay (F1)", &c.show_debug_overlay);
    }
    if (ImGui::CollapsingHeader("Audio", ImGuiTreeNodeFlags_DefaultOpen)) {
        int a = int(c.audio);
        const char* names[] = {"XAudio2", "SDL3", "Disabled"};
        if (ImGui::Combo("Audio backend", &a, names, 3))
            c.audio = core::AudioBackend(a);
        ImGui::SliderInt("Volume", &c.volume, 0, 100, "%d%%");
    }
    if (ImGui::CollapsingHeader("Input", ImGuiTreeNodeFlags_DefaultOpen)) {
        int i = int(c.input);
        const char* names[] = {"XInput (Xbox controllers)", "SDL3 (PlayStation, Switch Pro, generic)"};
        if (ImGui::Combo("Controller backend", &i, names, 2))
            c.input = core::InputBackend(i);
        ImGui::TextDisabled("Keyboard: WASD move, L/K/I/J = A/B/X/Y, Q/E = L/R, 1/3 = ZL/ZR, Enter = +, Tab = -");
    }
    if (ImGui::CollapsingHeader("System", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Checkbox("Docked mode (1080p)", &c.docked);
        const char* langs[] = {"Japanese", "English (US)", "French", "German", "Italian", "Spanish", "Chinese",
                               "Korean", "Dutch", "Portuguese", "Russian", "Taiwanese", "English (UK)",
                               "French (Canada)", "Spanish (Latin America)", "Chinese (Simplified)",
                               "Chinese (Traditional)"};
        ImGui::Combo("Language", &c.language, langs, 17);
        const char* levels[] = {"Trace", "Debug", "Info", "Warning", "Error"};
        ImGui::Combo("Log level", &c.log_level, levels, 5);
        PathInput("Save folder", c.save_dir);
    }
    ImGui::Spacing();
    if (ImGui::Button("Save settings", ImVec2(200, 36)))
        st.status = c.Save(kIni) ? "Settings saved." : "Could not write mua3.ini";
    ImGui::SameLine();
    if (ImGui::Button("Reset to defaults", ImVec2(200, 36))) {
        auto paths = std::make_tuple(c.game_dir, c.isos_dir, c.keys_dir, c.save_dir);
        c = core::Config{};
        std::tie(c.game_dir, c.isos_dir, c.keys_dir, c.save_dir) = paths;
    }
}

void AboutTab() {
    ImGui::Spacing();
    ImGui::TextWrapped(
        "A private native Windows port of Marvel Ultimate Alliance 3 built from your own dumps. The game code "
        "currently runs on an AArch64 JIT with high-level emulation of the Switch OS; functions are being replaced "
        "with native implementations step by step.");
    ImGui::Spacing();
    ImGui::BulletText("Renderer: Direct3D 11");
    ImGui::BulletText("Audio: XAudio2 or SDL3");
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
    st.Refresh();

    core::Config wcfg;
    wcfg.width = 980;
    wcfg.height = 680;
    wcfg.vsync = true;
    platform::Display display;
    if (!display.Init(wcfg, "MUA3 Launcher"))
        return 1;
    ApplyTheme();
    if (fs::exists("C:\\Windows\\Fonts\\segoeui.ttf")) {
        ImGui::GetIO().Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 19.0f);
    }
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
            if (ImGui::BeginTabItem("Settings")) {
                SettingsTab(st);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("About")) {
                AboutTab();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        if (!st.status.empty()) {
            ImGui::SetCursorPosY(ImGui::GetWindowHeight() - 34);
            ImGui::TextDisabled("%s", st.status.c_str());
        }
        ImGui::End();
    };

    while (display.PumpEvents()) {
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
