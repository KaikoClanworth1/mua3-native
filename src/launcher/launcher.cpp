// MUA3 Launcher: install from dumps, configure, and start the game.
#include <fstream>
#include <optional>
#include <thread>

#include <d3d11.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <wincodec.h>
#include <windows.h>

#include <SDL3/SDL.h>
#include <imgui.h>

#include "core/config.h"
#include "common/crash.h"
#include "launcher/game_builder.h"
#include "launcher/report.h"
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
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED); // (already done by wWinMain)
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
    c[ImGuiCol_TitleBg] = red_lo;
    c[ImGuiCol_TitleBgActive] = red;
    c[ImGuiCol_PopupBg] = ImVec4(0.09f, 0.09f, 0.11f, 1.0f);
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

HWND g_hwnd = nullptr; // the launcher window: owner of the file dialogs

// Windows file dialogs, modal, on the launcher's own (single-threaded COM, see wWinMain) UI thread. (Run on a helper
// thread while the UI thread waited, the dialog's messages to its owner window could never be answered: the launcher
// froze, "not responding", as soon as Add files was pressed.)
std::vector<fs::path> RunFileDialog(bool folder, bool multi, const wchar_t* title,
                                    std::vector<std::pair<std::wstring, std::wstring>> filters, const fs::path& start) {
    std::vector<fs::path> out;
    LOG_INFO("Launcher", "file dialog: {}", fs::path(title).string());
    {
        IFileOpenDialog* dlg = nullptr;
        if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) {
            DWORD opts = 0;
            dlg->GetOptions(&opts);
            opts |= FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | (folder ? FOS_PICKFOLDERS : FOS_FILEMUSTEXIST) |
                    (multi ? FOS_ALLOWMULTISELECT : 0);
            dlg->SetOptions(opts);
            dlg->SetTitle(title);
            std::vector<COMDLG_FILTERSPEC> spec;
            for (auto& f : filters)
                spec.push_back({f.first.c_str(), f.second.c_str()});
            if (!spec.empty())
                dlg->SetFileTypes(UINT(spec.size()), spec.data());
            IShellItem* dir = nullptr;
            std::error_code ec;
            if (!start.empty() && fs::is_directory(start, ec) &&
                SUCCEEDED(SHCreateItemFromParsingName(fs::absolute(start).wstring().c_str(), nullptr, IID_PPV_ARGS(&dir)))) {
                dlg->SetFolder(dir);
                dir->Release();
            }
            if (SUCCEEDED(dlg->Show(g_hwnd))) {
                IShellItemArray* items = nullptr;
                if (SUCCEEDED(dlg->GetResults(&items))) {
                    DWORD n = 0;
                    items->GetCount(&n);
                    for (DWORD i = 0; i < n; i++) {
                        IShellItem* it = nullptr;
                        PWSTR path = nullptr;
                        if (SUCCEEDED(items->GetItemAt(i, &it)) && SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                            out.emplace_back(path);
                            CoTaskMemFree(path);
                        }
                        if (it)
                            it->Release();
                    }
                    items->Release();
                }
            }
            dlg->Release();
        }
    }
    LOG_INFO("Launcher", "file dialog: {} chosen", out.size());
    return out;
}

// Test hook (MUA3_LAUNCHER_TEST_DIALOG): moves this process's file dialog off screen when it appears and cancels it.
void CancelTestDialog() {
    std::thread([] {
        struct Find {
            DWORD pid;
            HWND h;
        };
        for (int k = 0; k < 1000; k++) {
            Sleep(10);
            Find f{GetCurrentProcessId(), nullptr};
            EnumWindows(
                [](HWND h, LPARAM lp) -> BOOL {
                    auto* f = reinterpret_cast<Find*>(lp);
                    DWORD p = 0;
                    GetWindowThreadProcessId(h, &p);
                    char cls[32]{};
                    GetClassNameA(h, cls, sizeof(cls));
                    if (p == f->pid && std::strcmp(cls, "#32770") == 0) {
                        f->h = h;
                        return FALSE;
                    }
                    return TRUE;
                },
                reinterpret_cast<LPARAM>(&f));
            if (f.h) {
                SetWindowPos(f.h, nullptr, -32000, -32000, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
                Sleep(1500);
                PostMessageW(f.h, WM_COMMAND, IDCANCEL, 0);
                return;
            }
        }
    }).detach();
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
    bool installed = false;
    loader::InstallProgress progress;
    std::thread worker;
    std::atomic<bool> installing{false};
    std::string install_result;
    std::string status;
    Texture icon;
    std::vector<Monitor> monitors;
    // install: what the user picked (pre-filled from the Isos folder and keys folder)
    std::vector<loader::DumpInfo> picked;
    fs::path keys_file;
    // build: mua3.exe made from the install (game_builder)
    builder::Progress build;
    std::thread build_worker;
    builder::Toolchain toolchain;
    double toolchain_checked = -100;
    int build_jobs = 0;
    // problem report
    bool open_report = false;          // open the form this frame
    std::optional<report::Crash> crash; // the newest crash, when not reported/dismissed yet
    u64 crash_seen = 0;                // launcher.ini: newest crash time already reported or dismissed
    char about[4096] = {};
    bool report_save = true; // include the save (to reach the problem again)
    report::Result last_report;
    // launcher.ini
    bool show_console = false;
    bool close_on_play = true;
    int ui_scale_pct = 0; // 0 = follow the monitor
    int muted_volume = -1; // volume to restore when unmuting

    void Refresh() {
        installed = loader::IsInstalled(cfg.game_dir);
    }
    // First start of the Install tab: whatever is already in the Isos and keys folders.
    void Prefill() {
        if (keys_file.empty() && fs::exists(cfg.keys_dir / "prod.keys"))
            keys_file = cfg.keys_dir / "prod.keys";
        if (picked.empty()) {
            std::error_code ec;
            for (auto& e : fs::directory_iterator(cfg.isos_dir, ec))
                AddDump(e.path());
        }
    }
    void AddDump(const fs::path& path) {
        auto ext = path.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (ext != ".nsp" && ext != ".xci")
            return;
        std::error_code ec;
        for (auto& d : picked)
            if (fs::equivalent(d.path, path, ec))
                return;
        picked.push_back(loader::ClassifyDump(path));
        // base game first, then the update, then DLC (by file name)
        std::stable_sort(picked.begin(), picked.end(), [](const loader::DumpInfo& a, const loader::DumpInfo& b) {
            auto rank = [](const loader::DumpInfo& d) { return d.base ? 0 : d.update ? 1 : d.dlc ? 2 : 3; };
            return rank(a) != rank(b) ? rank(a) < rank(b) : a.path.filename() < b.path.filename();
        });
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
            else if (k == "crash_seen") crash_seen = std::strtoull(line.c_str() + eq + 1, nullptr, 10);
        }
    }
    void SavePrefs() const {
        std::ofstream f(kLauncherIni);
        f << "show_console=" << show_console << "\nclose_on_play=" << close_on_play << "\nui_scale=" << ui_scale_pct
          << "\nmuted_volume=" << muted_volume << "\ncrash_seen=" << crash_seen << "\n";
    }
    const Monitor* Primary() const { return monitors.empty() ? nullptr : &monitors[0]; }
};

// ---- presets: one click for the common setups; anything else shows as Custom.
enum class Preset { Recommended, Smooth, HighRefresh, Custom };
int HighRefreshRate(const LauncherState& st) {
    const Monitor* mon = st.Primary();
    return mon ? RecommendedRate(mon->refresh) : 60;
}
Preset CurrentPreset(const LauncherState& st) {
    const auto& c = st.cfg;
    if (c.resolution_scale != 0 || !c.vsync || c.fps_limit != 0 || c.swap_interval != 0)
        return Preset::Custom;
    if (c.fps_target == 30)
        return Preset::Recommended;
    if (c.fps_target == 60)
        return Preset::Smooth;
    if (c.fps_target == HighRefreshRate(st))
        return Preset::HighRefresh;
    return Preset::Custom;
}
const char* PresetName(const LauncherState& st) {
    switch (CurrentPreset(st)) {
    case Preset::Recommended: return "Recommended";
    case Preset::Smooth: return "Smooth";
    case Preset::HighRefresh: return "High refresh";
    default: return "Custom";
    }
}
void ApplyPreset(LauncherState& st, Preset p) {
    auto& c = st.cfg;
    c.resolution_scale = 0; // sharp at the display's resolution
    c.vsync = true;
    c.fps_limit = 0;
    c.swap_interval = 0;
    c.fps_target = p == Preset::Recommended ? 30 : p == Preset::Smooth ? 60 : HighRefreshRate(st);
}

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
    ImGui::TextDisabled("Native Windows build %s  |  D3D11  |  XAudio2/SDL  |  XInput/SDL", common::Version());
    ImGui::Text("Game data: %s", st.installed ? "installed" : "not installed");
    ImGui::EndGroup();
    ImGui::SameLine(ImGui::GetWindowWidth() - S(230));
    if (ImGui::Button("Report a problem", S(210, 36)))
        st.open_report = true;
    ImGui::Separator();
}

// "Report a problem": what happened, then a zip of logs, crash files and system information.
void ReportWindow(LauncherState& st) {
    if (st.open_report) {
        ImGui::OpenPopup("Report a problem");
        st.open_report = false;
        st.last_report = {};
    }
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.5f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(std::min(vp->Size.x - S(40), S(760)), std::min(vp->Size.y - S(40), S(600))), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("Report a problem", nullptr, ImGuiWindowFlags_NoSavedSettings))
        return;
    if (st.crash)
        ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.35f, 1), "Newest crash: %s", st.crash->headline.c_str());
    ImGui::TextWrapped("What happened? What were you doing (chapter, character, menu, button), and what did you see?");
    ImGui::InputTextMultiline("##about", st.about, sizeof(st.about), ImVec2(-1, S(150)));
    const builder::Paths paths = builder::DefaultPaths(st.cfg.game_dir);
    ImGui::Checkbox("Include my save, so the problem can be reached again from the same point", &st.report_save);
    const fs::path save = st.report_save ? st.cfg.save_dir : fs::path();
    const auto items = report::Collect(st.cfg.game_dir, paths.work_dir, save);
    u64 total = 0;
    for (auto& it : items)
        total += it.size;
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("Included (%zu files, %.1f MB): your text, system and version info, settings, logs, crash files, "
                       "the build log%s. Never your keys or game files.", items.size() + 1, total / 1048576.0,
                       st.report_save ? ", your save" : "");
    ImGui::PopStyleColor();
    if (ImGui::TreeNode("Files")) {
        for (auto& it : items)
            ImGui::BulletText("%s  (%.1f KB)", it.name.c_str(), it.size / 1024.0);
        ImGui::TreePop();
    }
    ImGui::Spacing();
    if (ImGui::Button("Create report", S(200, 40))) {
        std::vector<std::string> extra;
        extra.push_back(fmt::format("Game built: {}", builder::GameBuilt(paths) ? "yes" : "no"));
        const auto tc = builder::FindToolchain();
        extra.push_back("C++ tools: " + (tc.ok ? "MSVC " + tc.version : tc.problem));
        for (auto& m : st.monitors)
            extra.push_back(fmt::format("Display {}: {} x {} @ {:.0f} Hz, {:.0f}% scale", m.name, m.w, m.h, m.refresh, m.scale * 100));
        st.last_report = report::Create(st.about, extra, st.cfg.game_dir, paths.work_dir, save);
        if (st.last_report.ok && st.crash) {
            st.crash_seen = st.crash->time;
            st.crash.reset();
            st.SavePrefs();
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Close", S(120, 40)))
        ImGui::CloseCurrentPopup();
    if (st.last_report.ok) {
        ImGui::TextColored(ImVec4(0.3f, 0.85f, 0.4f, 1), "Saved: %s", st.last_report.zip.string().c_str());
        ImGui::TextWrapped("Attach this zip to a new issue on GitHub (or send it to whoever asked for it).");
        if (ImGui::Button("Show the file", S(180, 34)))
            ShellExecuteW(nullptr, L"open", L"explorer.exe", (L"/select,\"" + st.last_report.zip.wstring() + L"\"").c_str(),
                          nullptr, SW_SHOWNORMAL);
        ImGui::SameLine();
        if (ImGui::Button("Open GitHub issues", S(220, 34)))
            ShellExecuteW(nullptr, L"open", L"https://github.com/KaikoClanworth1/mua3-native/issues/new", nullptr, nullptr,
                          SW_SHOWNORMAL);
    } else if (!st.last_report.error.empty()) {
        ImGui::TextColored(ImVec4(0.95f, 0.35f, 0.3f, 1), "%s", st.last_report.error.c_str());
    }
    ImGui::EndPopup();
}

// Internal render size the game will use for these settings (mirrors main.cpp's auto scale).
std::pair<int, int> RenderSize(const core::Config& c) {
    const int base_h = 1080, base_w = 1920; // docked output (handheld mode is not offered)
    int scale = c.resolution_scale;
    if (scale <= 0)
        scale = (c.height + base_h - 1) / base_h;
    scale = std::clamp(scale, 1, 4);
    return {base_w * scale, base_h * scale};
}

void PlayTab(LauncherState& st) {
    ImGui::Spacing();
    if (st.crash) {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.30f, 0.08f, 0.08f, 1));
        ImGui::BeginChild("crash", ImVec2(0, S(96)), ImGuiChildFlags_Borders);
        ImGui::TextColored(ImVec4(1, 0.6f, 0.5f, 1), "The game closed unexpectedly.");
        ImGui::TextDisabled("%s", st.crash->headline.c_str());
        if (ImGui::Button("Report a problem", S(200, 32)))
            st.open_report = true;
        ImGui::SameLine();
        if (ImGui::Button("Dismiss", S(120, 32))) {
            st.crash_seen = st.crash->time;
            st.crash.reset();
            st.SavePrefs();
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::Spacing();
    }
    if (!st.installed) {
        ImGui::TextWrapped("The game data hasn't been installed yet. Go to the Install tab to install your copy.");
        return;
    }
    if (!fs::exists(builder::DefaultPaths(st.cfg.game_dir).out_exe)) {
        ImGui::TextWrapped("The game hasn't been built yet. Go to the Install tab, step 5, to build it from your install.");
        return;
    }
    const auto& c = st.cfg;
    ImGui::BeginChild("play", ImVec2(0, S(170)), ImGuiChildFlags_Borders);
    const auto [rw, rh] = RenderSize(c);
    ImGui::Text("Display: %d x %d %s   |   renders at %d x %d", c.width, c.height, c.fullscreen ? "fullscreen" : "window",
                rw, rh);
    ImGui::Text("Frame rate: %d fps%s   |   Preset: %s", c.fps_target, c.fps_target > 60 ? " (experimental)" : "",
                PresetName(st));
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
        st.cfg.docked = true;
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

std::string FreeSpace(const fs::path& p) {
    std::error_code ec;
    fs::path probe = fs::absolute(p, ec);
    while (!probe.empty() && !fs::exists(probe, ec) && probe != probe.parent_path())
        probe = probe.parent_path();
    const auto sp = fs::space(probe, ec);
    return ec ? std::string("?") : fmt::format("{:.1f} GB free", double(sp.available) / (1ull << 30));
}

void StepTitle(int n, const char* text, bool done) {
    ImGui::Spacing();
    ImGui::TextColored(done ? ImVec4(0.3f, 0.85f, 0.4f, 1) : ImVec4(0.95f, 0.75f, 0.3f, 1), done ? "  %d  " : "  %d  ", n);
    ImGui::SameLine();
    ImGui::TextUnformatted(text);
}

// Step 5: build mua3.exe from the installed game (game_builder): the C++ tools, then translate, compile and link.
void BuildStep(LauncherState& st) {
    const builder::Paths paths = builder::DefaultPaths(st.cfg.game_dir);
    const bool building = st.build.running;
    const bool built = builder::GameBuilt(paths);
    StepTitle(5, built ? "Game built. Build again after updating this launcher." : "Build the game from your install", built);
    ImGui::Indent(S(44));
    if (!fs::exists(paths.sdk_dir)) {
        ImGui::TextDisabled("This copy of the launcher has no sdk folder (a developer build): build mua3 with CMake.");
        ImGui::Unindent(S(44));
        return;
    }
    if (!building && ImGui::GetTime() - st.toolchain_checked > 10) { // vswhere is quick; refresh now and then
        st.toolchain = builder::FindToolchain();
        st.toolchain_checked = ImGui::GetTime();
    }
    if (st.build_jobs <= 0)
        st.build_jobs = builder::DefaultJobs();
    if (!st.toolchain.ok) {
        ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.3f, 1), "%s", st.toolchain.problem.c_str());
        ImGui::TextWrapped("The game is compiled on your PC with Microsoft's free C++ compiler. Install \"Build Tools for "
                           "Visual Studio 2026\" with the \"Desktop development with C++\" workload, then come back here.");
        if (ImGui::Button("Get the Build Tools", S(240, 34)))
            ShellExecuteW(nullptr, L"open", L"https://visualstudio.microsoft.com/downloads/#build-tools-for-visual-studio",
                          nullptr, nullptr, SW_SHOWNORMAL);
        ImGui::Unindent(S(44));
        return;
    }
    ImGui::TextDisabled("Microsoft C++ %s found. The first build takes about an hour (8 cores) and needs ~5 GB.",
                        st.toolchain.version.c_str());
    if (building) {
        const int done = st.build.done, total = st.build.total;
        ImGui::Text("%s", st.build.GetStage().c_str());
        if (total > 0)
            ImGui::ProgressBar(float(done) / float(total), ImVec2(-S(20), 0), fmt::format("{} / {}", done, total).c_str());
        else
            ImGui::ProgressBar(-1.0f * float(ImGui::GetTime()), ImVec2(-S(20), 0), "working...");
        if (ImGui::Button("Stop", S(120, 34)))
            st.build.cancel = true;
        ImGui::SameLine();
        ImGui::TextDisabled("(finished parts are kept: building again carries on)");
    } else {
        ImGui::SetNextItemWidth(S(240));
        ImGui::SliderInt("Compiler jobs", &st.build_jobs, 1, std::max(1, int(std::thread::hardware_concurrency())));
        Help("How many parts compile at once. Each needs about 2.5 GB of memory. Fewer jobs leave the PC more usable "
             "while it builds.");
        ImGui::BeginDisabled(!st.installed);
        if (ImGui::Button(built ? "Build again" : "Build the game", S(260, 44))) {
            if (st.build_worker.joinable())
                st.build_worker.join();
            st.build.cancel = false;
            {
                std::lock_guard lk(st.build.mutex);
                st.build.error.clear();
                st.build.log.clear();
            }
            st.build.running = true;
            const int jobs = st.build_jobs;
            st.build_worker = std::thread([&st, paths, jobs] { builder::BuildGame(paths, jobs, st.build); });
        }
        ImGui::EndDisabled();
        if (st.build.finished) {
            const std::string err = st.build.GetError();
            if (st.build.ok)
                ImGui::TextColored(ImVec4(0.3f, 0.85f, 0.4f, 1), "%s Go to Play.", st.build.GetStage().c_str());
            else if (!err.empty())
                ImGui::TextColored(ImVec4(0.95f, 0.35f, 0.3f, 1), "%s", err.c_str());
        }
    }
    if (building || st.build.finished) {
        ImGui::BeginChild("buildlog", ImVec2(0, S(110)), ImGuiChildFlags_Borders);
        for (auto& line : st.build.Tail(30))
            ImGui::TextUnformatted(line.c_str());
        if (building)
            ImGui::SetScrollHereY(1.0f);
        ImGui::EndChild();
    }
    ImGui::Unindent(S(44));
}

void InstallTab(LauncherState& st) {
    st.Prefill();
    const bool busy = st.installing;
    ImGui::BeginDisabled(busy);

    // 1. keys
    const bool keys_ok = !st.keys_file.empty() && fs::exists(st.keys_file);
    StepTitle(1, "Your console keys (prod.keys)", keys_ok);
    ImGui::Indent(S(44));
    ImGui::TextDisabled("%s", keys_ok ? st.keys_file.string().c_str() : "Not chosen yet. Dump them from your own Switch with Lockpick_RCM.");
    if (ImGui::Button("Choose prod.keys...", S(220, 34))) {
        auto f = RunFileDialog(false, false, L"Choose your prod.keys", {{L"Switch keys (*.keys)", L"*.keys"}, {L"All files", L"*.*"}},
                               st.keys_file.empty() ? st.cfg.keys_dir : st.keys_file.parent_path());
        if (!f.empty())
            st.keys_file = f[0];
    }
    ImGui::Unindent(S(44));

    // 2. game files
    bool has_base = false, has_update = false;
    int dlc = 0, other = 0;
    for (auto& d : st.picked) {
        has_base |= d.base;
        has_update |= d.update;
        dlc += d.dlc;
        other += !d.ours;
    }
    StepTitle(2, "Game files: the base game, the update and any DLC packs (.nsp / .xci)", has_base);
    ImGui::Indent(S(44));
    if (ImGui::Button("Add files...", S(160, 34))) {
        auto files = RunFileDialog(false, true, L"Choose the base game, update and DLC files",
                                   {{L"Switch games (*.nsp; *.xci)", L"*.nsp;*.xci"}, {L"All files", L"*.*"}},
                                   st.picked.empty() ? st.cfg.isos_dir : st.picked.back().path.parent_path());
        for (auto& f : files)
            st.AddDump(f);
    }
    ImGui::SameLine();
    if (ImGui::Button("Add a folder...", S(160, 34))) {
        auto dirs = RunFileDialog(true, false, L"Choose a folder with your game files", {}, st.cfg.isos_dir);
        std::error_code ec;
        if (!dirs.empty())
            for (auto& e : fs::directory_iterator(dirs[0], ec))
                st.AddDump(e.path());
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear", S(100, 34)))
        st.picked.clear();
    if (ImGui::BeginTable("dumps", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY,
                          ImVec2(0, S(150)))) {
        ImGui::TableSetupColumn("What", ImGuiTableColumnFlags_WidthFixed, S(150));
        ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, S(90));
        ImGui::TableSetupColumn("File", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, S(40));
        ImGui::TableHeadersRow();
        int remove = -1;
        for (int k = 0; k < int(st.picked.size()); k++) {
            const auto& d = st.picked[k];
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            const ImVec4 col = !d.ours ? ImVec4(0.95f, 0.35f, 0.3f, 1) : d.kind == "unknown" ? ImVec4(0.95f, 0.75f, 0.3f, 1)
                                                                                           : ImVec4(0.85f, 0.85f, 0.9f, 1);
            ImGui::TextColored(col, "%s", d.kind.c_str());
            ImGui::TableNextColumn();
            if (d.size >= (100ull << 20))
                ImGui::Text("%.2f GB", double(d.size) / (1ull << 30));
            else
                ImGui::Text("%.1f MB", double(d.size) / (1ull << 20));
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(d.path.filename().string().c_str());
            ImGui::SetItemTooltip("%s", d.path.string().c_str());
            ImGui::TableNextColumn();
            ImGui::PushID(k);
            if (ImGui::SmallButton(" x "))
                remove = k;
            ImGui::PopID();
        }
        if (remove >= 0)
            st.picked.erase(st.picked.begin() + remove);
        ImGui::EndTable();
    }
    auto check = [](bool ok, bool optional, const std::string& text) {
        ImGui::TextColored(ok ? ImVec4(0.3f, 0.85f, 0.4f, 1) : optional ? ImVec4(0.6f, 0.6f, 0.65f, 1) : ImVec4(0.95f, 0.35f, 0.3f, 1),
                           ok ? "OK" : optional ? "--" : "!!");
        ImGui::SameLine();
        ImGui::TextUnformatted(text.c_str());
    };
    check(has_base, false, "Base game");
    ImGui::SameLine(S(260));
    check(has_update, true, "Update (4.0.1 recommended)");
    ImGui::SameLine(S(560));
    check(dlc > 0, true, fmt::format("DLC packs: {}", dlc));
    if (other)
        ImGui::TextColored(ImVec4(0.95f, 0.35f, 0.3f, 1), "%d file(s) are not Marvel Ultimate Alliance 3: remove them.", other);
    ImGui::Unindent(S(44));

    // 3. destination
    const bool dest_ok = !st.cfg.game_dir.empty();
    StepTitle(3, "Install folder", dest_ok);
    ImGui::Indent(S(44));
    ImGui::TextDisabled("%s   (%s, the game needs about 18 GB)", fs::absolute(st.cfg.game_dir).string().c_str(),
                        FreeSpace(st.cfg.game_dir).c_str());
    if (ImGui::Button("Choose folder...", S(200, 34))) {
        auto dirs = RunFileDialog(true, false, L"Choose where to install the game data", {}, st.cfg.game_dir.parent_path());
        if (!dirs.empty())
            st.cfg.game_dir = dirs[0] / "game";
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(a \"game\" folder is made inside the one you choose)");
    ImGui::Unindent(S(44));
    ImGui::EndDisabled();

    // 4. install
    StepTitle(4, st.installed ? "Installed. Reinstall if you add an update or DLC later." : "Install", st.installed);
    ImGui::Indent(S(44));
    if (busy) {
        const u64 t = st.progress.bytes_total, d = st.progress.bytes_done;
        const float f = t ? float(double(d) / double(t)) : 0.0f;
        ImGui::Text("%s", st.progress.Stage().c_str());
        ImGui::ProgressBar(f, ImVec2(-S(20), 0), fmt::format("{:.2f} / {:.2f} GB", d / 1073741824.0, t / 1073741824.0).c_str());
        if (ImGui::Button("Cancel", S(120, 34)))
            st.progress.cancel = true;
    } else {
        ImGui::BeginDisabled(!keys_ok || !has_base || other > 0 || !dest_ok);
        if (ImGui::Button(st.installed ? "Reinstall game data" : "Install game data", S(260, 44))) {
            if (st.keys_file.filename() == "prod.keys")
                st.cfg.keys_dir = st.keys_file.parent_path();
            st.cfg.isos_dir = st.picked.front().path.parent_path();
            st.cfg.Save(kIni);
            st.installing = true;
            st.install_result.clear();
            st.progress.cancel = false;
            st.progress.bytes_done = 0;
            st.progress.bytes_total = 0;
            if (st.worker.joinable())
                st.worker.join();
            loader::InstallOptions o{st.cfg.isos_dir, st.cfg.keys_dir, st.cfg.game_dir, true};
            for (auto& d : st.picked)
                o.files.push_back(d.path);
            o.keys_file = st.keys_file;
            st.worker = std::thread([&st, o] {
                std::string err;
                const bool ok = loader::InstallGame(o, st.progress, err);
                st.install_result = ok ? "Install complete. Go to Play." : "Install failed: " + err;
                st.installing = false;
                st.Refresh();
            });
        }
        ImGui::EndDisabled();
        if (!st.install_result.empty())
            ImGui::TextWrapped("%s", st.install_result.c_str());
    }
    ImGui::Unindent(S(44));
    BuildStep(st);
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
    Help("The game draws at 1080p. Auto renders at the smallest multiple that covers your display, so "
         "1440p and 4K screens get a sharp image instead of an upscaled 1080p one. Higher is sharper and slower.");
}

void FrameRateSettings(LauncherState& st) {
    auto& c = st.cfg;
    int cur = 1;
    for (int i = 0; i < int(std::size(kRates)); i++)
        if (c.fps_target == kRates[i])
            cur = i;
    std::vector<std::string> names;
    for (int r : kRates)
        names.push_back(r == 30 ? "30 (original)" : r <= 60 ? std::to_string(r) : fmt::format("{} (experimental)", r));
    if (ImGui::BeginCombo("Frame rate", names[cur].c_str())) {
        for (int i = 0; i < int(std::size(kRates)); i++)
            if (ImGui::Selectable(names[i].c_str(), i == cur))
                c.fps_target = kRates[i];
        ImGui::EndCombo();
    }
    Help("30 is the game as it shipped. 60 is smoother and fully supported by the engine. Above 60 changes the game's "
         "timing and may run faster in places. For smooth motion pick a rate that divides your monitor's refresh rate.");
    ImGui::Checkbox("Show FPS", &c.show_fps);
}

// Languages the game ships (its control data), in set's numbering (mua3.ini [system] language).
constexpr std::pair<int, const char*> kLanguages[] = {
    {1, "English (US)"}, {12, "English (UK)"}, {2, "French"},  {13, "French (Canada)"}, {3, "German"},
    {4, "Italian"},      {5, "Spanish"},       {14, "Spanish (Latin America)"},         {0, "Japanese"},
    {7, "Korean"},       {15, "Chinese (Simplified)"},         {16, "Chinese (Traditional)"},
};

void SettingsTab(LauncherState& st) {
    auto& c = st.cfg;
    ImGui::Spacing();
    // ---- preset
    ImGui::TextUnformatted("Preset");
    const Preset cur = CurrentPreset(st);
    struct Choice {
        Preset p;
        const char* name;
        std::string text;
    };
    const Choice choices[] = {
        {Preset::Recommended, "Recommended", "The game as it shipped: 30 fps, VSync, sharp image at your display's resolution."},
        {Preset::Smooth, "Smooth", "60 fps. The engine supports it; motion is twice as smooth."},
        {Preset::HighRefresh, "High refresh", fmt::format("{} fps, matched to your monitor (experimental above 60).", HighRefreshRate(st))},
    };
    for (const Choice& ch : choices) {
        const bool on = cur == ch.p;
        if (on)
            ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        if (ImGui::Button(ch.name, S(170, 40)))
            ApplyPreset(st, ch.p);
        if (on)
            ImGui::PopStyleColor();
        ImGui::SetItemTooltip("%s", ch.text.c_str());
        ImGui::SameLine();
    }
    ImGui::NewLine();
    ImGui::TextDisabled("%s", cur == Preset::Custom ? "Custom: your own settings below."
                              : cur == Preset::Recommended ? choices[0].text.c_str()
                              : cur == Preset::Smooth ? choices[1].text.c_str() : choices[2].text.c_str());
    ImGui::Separator();

    if (ImGui::CollapsingHeader("Display", ImGuiTreeNodeFlags_DefaultOpen)) {
        DisplaySettings(st);
        FrameRateSettings(st);
    }
    if (ImGui::CollapsingHeader("Controls", ImGuiTreeNodeFlags_DefaultOpen)) {
        int i = int(c.input);
        const char* names[] = {"XInput (Xbox controllers)", "SDL3 (PlayStation, Switch Pro, generic)"};
        if (ImGui::Combo("Controller", &i, names, 2))
            c.input = core::InputBackend(i);
        ImGui::SliderInt("Camera speed", &c.camera_speed, 10, 200, "%d%%");
        Help("How fast the right stick turns the camera. 100% is the game's own speed; lower it for finer control.");
        ImGui::TextDisabled("Keyboard: WASD move, L/K/I/J = A/B/X/Y, Q/E = L/R, 1/3 = ZL/ZR, Enter = +, Tab = -");
    }
    if (ImGui::CollapsingHeader("Game", ImGuiTreeNodeFlags_DefaultOpen)) {
        int sel = 0;
        for (int k = 0; k < int(std::size(kLanguages)); k++)
            if (kLanguages[k].first == c.language)
                sel = k;
        if (ImGui::BeginCombo("Language", kLanguages[sel].second)) {
            for (int k = 0; k < int(std::size(kLanguages)); k++)
                if (ImGui::Selectable(kLanguages[k].second, k == sel))
                    c.language = kLanguages[k].first;
            ImGui::EndCombo();
        }
        Help("Text and voice language of the game. Takes effect the next time the game starts.");
    }
    if (ImGui::CollapsingHeader("Audio", ImGuiTreeNodeFlags_DefaultOpen)) {
        int a = int(c.audio);
        const char* names[] = {"XAudio2", "SDL3", "Disabled"};
        if (ImGui::Combo("Audio output", &a, names, 3))
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
    if (ImGui::CollapsingHeader("Advanced")) {
        ImGui::Checkbox("VSync", &c.vsync);
        ImGui::SameLine();
        ImGui::Checkbox("Debug overlay (F1)", &c.show_debug_overlay);
        ImGui::SliderInt("FPS limit", &c.fps_limit, 0, 240, c.fps_limit <= 0 ? "Off" : "%d");
        Help("Caps how fast frames are presented when the game is not limiting itself. 0 = off.");
        int si = c.swap_interval;
        const char* sis[] = {"Game decides", "Every frame (1)", "Every 2nd vblank (2)"};
        if (ImGui::Combo("Present interval", &si, sis, 3))
            c.swap_interval = si;
        const char* levels[] = {"Trace", "Debug", "Info", "Warning", "Error"};
        ImGui::Combo("Log level", &c.log_level, levels, 5);
        PathInput("Save folder", c.save_dir);
        const int pcts[] = {0, 100, 125, 150, 175, 200, 250};
        int ui = 0;
        for (int k = 0; k < 7; k++)
            if (st.ui_scale_pct == pcts[k])
                ui = k;
        const char* sizes[] = {"Auto (follow Windows)", "100%", "125%", "150%", "175%", "200%", "250%"};
        if (ImGui::Combo("Launcher size", &ui, sizes, 7)) {
            st.ui_scale_pct = pcts[ui];
            st.SavePrefs();
            st.status = "Launcher size changes on the next start.";
        }
    }
    ImGui::Spacing();
    if (ImGui::Button("Save settings", S(200, 36))) {
        c.docked = true;
        st.status = c.Save(kIni) ? "Settings saved." : "Could not write mua3.ini";
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset to Recommended", S(240, 36))) {
        auto keep = std::make_tuple(c.game_dir, c.isos_dir, c.keys_dir, c.save_dir, c.language, c.volume, c.input);
        c = core::Config{};
        std::tie(c.game_dir, c.isos_dir, c.keys_dir, c.save_dir, c.language, c.volume, c.input) = keep;
        if (const Monitor* mon = st.Primary()) { // sensible window size for this machine
            c.width = std::min(mon->w, 1920);
            c.height = std::min(mon->h, 1080);
        }
        ApplyPreset(st, Preset::Recommended);
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
    // The UI thread is a single-threaded COM apartment: the shell's file dialogs need one (WIC works in it too).
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
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
    common::RotateLog("launcher", 3);
    common::SetLogFile("launcher.log");
    common::InstallCrashHandler("launcher");
    LOG_INFO("Launcher", "MUA3 Native launcher {} in {}", common::Version(), fs::current_path().string());
    LauncherState st;
    st.cfg.Load(kIni);
    {
        int argc = 0;
        wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        bool build = false;
        int jobs = builder::DefaultJobs();
        for (int i = 1; i < argc; i++) {
            if (std::wstring(argv[i]) == L"--build-game")
                build = true;
            else if (std::wstring(argv[i]) == L"--jobs" && i + 1 < argc)
                jobs = std::max(1, _wtoi(argv[++i]));
            else if (std::wstring(argv[i]) == L"--crash-report") // started by the game after a crash
                st.open_report = true;
        }
        LocalFree(argv);
        if (build) { // unattended build (scripts, tests): progress in build_game/build.log
            builder::Progress prog;
            return builder::BuildGame(builder::DefaultPaths(st.cfg.game_dir), jobs, prog) ? 0 : 1;
        }
    }
    st.cfg.docked = true; // the PC build always runs as docked (1080p)
    st.LoadPrefs();
    st.crash = report::NewestCrash();
    if (st.crash && st.crash->time <= st.crash_seen)
        st.crash.reset();
    st.Refresh();

    core::Config wcfg;
    wcfg.width = 980;
    wcfg.height = 720;
    wcfg.vsync = true;
    platform::Display display;
    if (!display.Init(wcfg, "Marvel Ultimate Alliance 3 Launcher"))
        return 1;
    display.SetIcon(st.cfg.game_dir / "control" / "icon_AmericanEnglish.dat");
    st.monitors = ListMonitors();

    // Size everything for the monitor the window opened on (or the user's override), then grow the window to
    // match and keep it on screen.
    SDL_Window* win = display.Window();
    g_hwnd = (HWND)SDL_GetPointerProperty(SDL_GetWindowProperties(win), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
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
            static const std::string want_tab = std::getenv("MUA3_LAUNCHER_TAB") ? std::getenv("MUA3_LAUNCHER_TAB") : "";
            static int tab_frames = 0;
            auto flag = [&](const char* name) {
                return want_tab == name && tab_frames < 3 ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
            };
            tab_frames++;
            if (ImGui::BeginTabItem("Play")) {
                PlayTab(st);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Install", nullptr, flag("Install"))) {
                ImGui::BeginChild("install", ImVec2(0, -S(40)));
                InstallTab(st);
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
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
        ReportWindow(st);
        if (!st.status.empty()) {
            ImGui::SetCursorPosY(ImGui::GetWindowHeight() - S(34));
            ImGui::TextDisabled("%s", st.status.c_str());
        }
        ImGui::End();
    };

    // Automation (MUA3_HEADLESS + MUA3_LAUNCHER_SHOT=<file.bmp>[, MUA3_LAUNCHER_TAB=<tab>]): save a frame, exit.
    const char* shot = std::getenv("MUA3_LAUNCHER_SHOT");
    // Test (MUA3_LAUNCHER_TEST_DIALOG=1): open the "Add files" dialog, move it off screen and cancel it, then exit
    // with 0 (a hang here is the bug this guards against).
    const bool test_dialog = std::getenv("MUA3_LAUNCHER_TEST_DIALOG") != nullptr;
    for (int frame = 0; display.PumpEvents(); frame++) {
        if (test_dialog && frame == 10)
            CancelTestDialog();
        if (frame == 5 && std::getenv("MUA3_LAUNCHER_TEST_REPORT")) { // test: write a problem report and exit
            const auto r = report::Create("Test report (automated).", {"test line"}, st.cfg.game_dir,
                                          builder::DefaultPaths(st.cfg.game_dir).work_dir, st.cfg.save_dir);
            LOG_INFO("Launcher", "test report {}: {}", r.ok ? "ok" : "failed", r.ok ? r.zip.string() : r.error);
            break;
        }
        if (test_dialog && frame == 12) {
            const auto files = RunFileDialog(false, true, L"test", {{L"Switch games (*.nsp; *.xci)", L"*.nsp;*.xci"}}, ".");
            LOG_INFO("Launcher", "test dialog closed ({} files)", files.size());
            break;
        }
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
    st.build.cancel = true;
    if (st.build_worker.joinable())
        st.build_worker.join();
    display.Shutdown();
    return 0;
}
