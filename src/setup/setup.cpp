// MUA3 Setup: the PC-game style installer that replaces the launcher. Three steps:
//   Game files: the player's own base game, update and DLC dumps (found on the PC, browsed or dropped), their keys,
//               where the game goes, and how much room that takes.
//   Build:      the game data is installed, then the game is translated to C++, compiled and linked on this PC
//               (launcher/game_builder), and its shaders prepared.
//   Play:       desktop and Start-menu shortcuts with the game's icon (made from the player's own files), an entry in
//               Windows "Apps" (repair, update, uninstall), then the game. Every setting lives in the game itself.
// Running it again on an installed game offers play, update/repair and uninstall.
#include <chrono>
#include <cmath>
#include <fstream>
#include <thread>

#include <d3d11.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <wincodec.h>
#include <windows.h>

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_internal.h> // (FindRenderedTextEnd)

#include "common/crash.h"
#include "core/config.h"
#include "launcher/game_builder.h"
#include "launcher/report.h"
#include "loader/installer.h"
#include "platform/platform.h"
#include "setup/install_ops.h"

namespace {

using Clock = std::chrono::steady_clock;
constexpr float kPi = 3.14159265f;

// ---- look
float g_ui = 1.0f;
float S(float px) { return px * g_ui; }
ImVec2 S(float x, float y) { return ImVec2(x * g_ui, y * g_ui); }

ImFont* g_regular = nullptr;
ImFont* g_semibold = nullptr;
constexpr float kText = 15.0f, kSmall = 13.0f, kTitle = 26.0f, kRing = 28.0f;

namespace col {
const ImU32 bg_top = IM_COL32(44, 46, 53, 255), bg_bottom = IM_COL32(29, 30, 35, 255);
const ImU32 panel = IM_COL32(255, 255, 255, 10), panel_border = IM_COL32(255, 255, 255, 22);
const ImU32 text = IM_COL32(235, 236, 240, 255), dim = IM_COL32(150, 153, 162, 255), faint = IM_COL32(95, 98, 106, 255);
const ImU32 accent = IM_COL32(61, 107, 240, 255), accent_hi = IM_COL32(86, 128, 255, 255);
const ImU32 green = IM_COL32(63, 178, 127, 255), red = IM_COL32(229, 83, 75, 255), amber = IM_COL32(227, 163, 59, 255);
} // namespace col
ImVec4 V(ImU32 c) { return ImGui::ColorConvertU32ToFloat4(c); }
ImU32 Alpha(ImU32 c, int a) { return (c & 0x00FFFFFF) | (ImU32(a) << 24); }

void ApplyTheme() {
    ImGuiStyle& s = ImGui::GetStyle();
    s = ImGuiStyle();
    s.WindowRounding = 0;
    s.WindowBorderSize = 0;
    s.WindowPadding = ImVec2(0, 0);
    s.FrameRounding = 8;
    s.ChildRounding = 10;
    s.PopupRounding = 8;
    s.GrabRounding = 6;
    s.ScrollbarRounding = 6;
    s.ScrollbarSize = 10;
    s.FramePadding = ImVec2(12, 8);
    s.ItemSpacing = ImVec2(10, 8);
    s.ChildBorderSize = 1;
    s.PopupBorderSize = 1;
    ImVec4* c = s.Colors;
    c[ImGuiCol_Text] = V(col::text);
    c[ImGuiCol_TextDisabled] = V(col::dim);
    c[ImGuiCol_WindowBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ChildBg] = V(col::panel);
    c[ImGuiCol_Border] = V(col::panel_border);
    c[ImGuiCol_PopupBg] = ImVec4(0.15f, 0.16f, 0.19f, 1.0f);
    c[ImGuiCol_ModalWindowDimBg] = ImVec4(0, 0, 0, 0.55f);
    c[ImGuiCol_FrameBg] = ImVec4(1, 1, 1, 0.06f);
    c[ImGuiCol_FrameBgHovered] = ImVec4(1, 1, 1, 0.10f);
    c[ImGuiCol_FrameBgActive] = ImVec4(1, 1, 1, 0.13f);
    c[ImGuiCol_Button] = ImVec4(1, 1, 1, 0.08f);
    c[ImGuiCol_ButtonHovered] = ImVec4(1, 1, 1, 0.14f);
    c[ImGuiCol_ButtonActive] = ImVec4(1, 1, 1, 0.18f);
    c[ImGuiCol_Header] = ImVec4(0.24f, 0.42f, 0.94f, 0.45f);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.24f, 0.42f, 0.94f, 0.65f);
    c[ImGuiCol_HeaderActive] = V(col::accent);
    c[ImGuiCol_CheckMark] = ImVec4(1, 1, 1, 1);
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab] = ImVec4(1, 1, 1, 0.15f);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(1, 1, 1, 0.25f);
    c[ImGuiCol_ScrollbarGrabActive] = ImVec4(1, 1, 1, 0.30f);
    c[ImGuiCol_Separator] = V(col::panel_border);
    s.ScaleAllSizes(g_ui);
}

void LoadFonts() {
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    const char* regular = "C:\\Windows\\Fonts\\segoeui.ttf";
    const char* semibold = "C:\\Windows\\Fonts\\seguisb.ttf";
    if (fs::exists(regular))
        g_regular = io.Fonts->AddFontFromFileTTF(regular, S(kText));
    if (!g_regular) {
        ImFontConfig fc;
        fc.SizePixels = S(13.0f);
        g_regular = io.Fonts->AddFontDefault(&fc);
    }
    g_semibold = fs::exists(semibold) ? io.Fonts->AddFontFromFileTTF(semibold, S(kText)) : g_regular;
    if (!g_semibold)
        g_semibold = g_regular;
}

struct FontScope {
    FontScope(ImFont* f, float size) { ImGui::PushFont(f, S(size)); }
    ~FontScope() { ImGui::PopFont(); }
};

struct Texture {
    ID3D11ShaderResourceView* srv = nullptr;
    int w = 0, h = 0;
};

Texture LoadImageTexture(ID3D11Device* dev, const fs::path& path) {
    Texture t;
    IWICImagingFactory* wic = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))))
        return t;
    IWICBitmapDecoder* dec = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* conv = nullptr;
    if (SUCCEEDED(wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &dec)) &&
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
            t.w = int(w), t.h = int(h);
        }
    }
    if (conv) conv->Release();
    if (frame) frame->Release();
    if (dec) dec->Release();
    wic->Release();
    return t;
}

HWND g_hwnd = nullptr;

std::vector<fs::path> FileDialog(bool folder, bool multi, const wchar_t* title,
                                 std::vector<std::pair<std::wstring, std::wstring>> filters, const fs::path& start) {
    std::vector<fs::path> out;
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg))))
        return out;
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    opts |= FOS_FORCEFILESYSTEM | (folder ? FOS_PICKFOLDERS : FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST) | (multi ? FOS_ALLOWMULTISELECT : 0);
    dlg->SetOptions(opts);
    dlg->SetTitle(title);
    std::vector<COMDLG_FILTERSPEC> spec;
    for (auto& f : filters)
        spec.push_back({f.first.c_str(), f.second.c_str()});
    if (!spec.empty())
        dlg->SetFileTypes(UINT(spec.size()), spec.data());
    std::error_code ec;
    fs::path dir = start;
    while (!dir.empty() && !fs::is_directory(dir, ec) && dir != dir.parent_path())
        dir = dir.parent_path();
    IShellItem* item = nullptr;
    if (!dir.empty() && SUCCEEDED(SHCreateItemFromParsingName(fs::absolute(dir).wstring().c_str(), nullptr, IID_PPV_ARGS(&item)))) {
        dlg->SetFolder(item);
        item->Release();
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
    return out;
}

// Languages the game ships, in the set service's numbering (mua3.ini [system] language).
constexpr std::pair<int, const char*> kLanguages[] = {
    {1, "English (US)"}, {12, "English (UK)"}, {2, "French"},  {13, "French (Canada)"}, {3, "German"},
    {4, "Italian"},      {5, "Spanish"},       {14, "Spanish (Latin America)"},         {0, "Japanese"},
    {7, "Korean"},       {15, "Chinese (Simplified)"},         {16, "Chinese (Traditional)"},
};
int WindowsLanguage() {
    const LANGID id = GetUserDefaultUILanguage();
    switch (PRIMARYLANGID(id)) {
    case LANG_ENGLISH: return SUBLANGID(id) == SUBLANG_ENGLISH_US ? 1 : 12;
    case LANG_FRENCH: return SUBLANGID(id) == SUBLANG_FRENCH_CANADIAN ? 13 : 2;
    case LANG_GERMAN: return 3;
    case LANG_ITALIAN: return 4;
    case LANG_SPANISH: return SUBLANGID(id) == SUBLANG_SPANISH_MODERN || SUBLANGID(id) == SUBLANG_SPANISH ? 5 : 14;
    case LANG_JAPANESE: return 0;
    case LANG_KOREAN: return 7;
    case LANG_CHINESE: return SUBLANGID(id) == SUBLANG_CHINESE_SIMPLIFIED ? 15 : 16;
    default: return 1;
    }
}

std::string Short(const fs::path& p, size_t max = 70) {
    std::string s = p.string();
    if (s.size() <= max)
        return s;
    return s.substr(0, 18) + "..." + s.substr(s.size() - (max - 21));
}

std::string Clock2(double seconds) {
    const int s = int(seconds);
    return s >= 3600 ? fmt::format("{}:{:02}:{:02}", s / 3600, s / 60 % 60, s % 60) : fmt::format("{}:{:02}", s / 60, s % 60);
}

// ---- widgets
void Label(const char* text) { // small caps section title
    FontScope f(g_semibold, kSmall);
    ImGui::PushStyleColor(ImGuiCol_Text, V(col::dim));
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
}

void Dim(const std::string& text, bool wrap = true) {
    ImGui::PushStyleColor(ImGuiCol_Text, V(col::dim));
    if (wrap)
        ImGui::TextWrapped("%s", text.c_str());
    else
        ImGui::TextUnformatted(text.c_str());
    ImGui::PopStyleColor();
}

bool Button(const char* label, ImVec2 size, bool primary = false, bool enabled = true) {
    FontScope f(g_semibold, kText);
    if (primary) {
        ImGui::PushStyleColor(ImGuiCol_Button, V(col::accent));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, V(col::accent_hi));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, V(col::accent_hi));
    }
    ImGui::BeginDisabled(!enabled);
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    if (primary && enabled) // soft glow
        for (int i = 3; i >= 1; i--)
            ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(pos.x - S(2.0f * i), pos.y - S(1.5f * i) + S(3)),
                                                      ImVec2(pos.x + size.x + S(2.0f * i), pos.y + size.y + S(2.0f * i) + S(3)),
                                                      Alpha(col::accent, 14), S(10 + 2.0f * i));
    const bool pressed = ImGui::Button(label, size);
    ImGui::EndDisabled();
    if (primary)
        ImGui::PopStyleColor(3);
    return pressed && enabled;
}

bool Link(const char* label, ImU32 color = col::text) {
    FontScope f(g_semibold, kText);
    ImGui::PushStyleColor(ImGuiCol_Text, V(color));
    const ImVec2 sz = ImGui::CalcTextSize(label, nullptr, true);
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const bool pressed = ImGui::InvisibleButton(label, sz);
    if (ImGui::IsItemHovered()) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        ImGui::GetWindowDrawList()->AddLine(ImVec2(pos.x, pos.y + sz.y), ImVec2(pos.x + sz.x, pos.y + sz.y), color);
    }
    ImGui::GetWindowDrawList()->AddText(pos, color, label, ImGui::FindRenderedTextEnd(label));
    ImGui::PopStyleColor();
    return pressed;
}

void Badge(const std::string& text, ImU32 color, ImVec2 right_center) {
    FontScope f(g_semibold, kSmall);
    const std::string& t = text;
    const ImVec2 sz = ImGui::CalcTextSize(t.c_str());
    const ImVec2 pad = S(10, 4);
    const ImVec2 a(right_center.x - sz.x - 2 * pad.x, right_center.y - sz.y * 0.5f - pad.y);
    const ImVec2 b(right_center.x, right_center.y + sz.y * 0.5f + pad.y);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(a, b, Alpha(color, 40), (b.y - a.y) * 0.5f);
    dl->AddRect(a, b, Alpha(color, 90), (b.y - a.y) * 0.5f);
    dl->AddText(ImVec2(a.x + pad.x, a.y + pad.y), color, t.c_str());
}

void CheckIcon(ImDrawList* dl, ImVec2 c, float r, ImU32 color) {
    dl->AddCircleFilled(c, r, Alpha(color, 50));
    dl->AddCircle(c, r, Alpha(color, 140), 0, S(1.2f));
    dl->PathLineTo(ImVec2(c.x - r * 0.42f, c.y + r * 0.02f));
    dl->PathLineTo(ImVec2(c.x - r * 0.10f, c.y + r * 0.34f));
    dl->PathLineTo(ImVec2(c.x + r * 0.45f, c.y - r * 0.30f));
    dl->PathStroke(color, 0, S(2.0f));
}

void Spinner(ImDrawList* dl, ImVec2 c, float r, ImU32 color) {
    const float t = float(ImGui::GetTime()) * 5.0f;
    dl->PathArcTo(c, r, t, t + 4.2f, 24);
    dl->PathStroke(color, 0, S(2.2f));
}

// A rounded panel: a child window with padding.
bool BeginPanel(const char* id, ImVec2 size, ImVec2 pad = ImVec2(16, 12)) {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, S(pad.x, pad.y));
    const bool open = ImGui::BeginChild(id, size, ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding |
                                                      (size.y == 0 ? ImGuiChildFlags_AutoResizeY : 0));
    ImGui::PopStyleVar();
    return open;
}
void EndPanel() { ImGui::EndChild(); }

// ---- the wizard
enum class Page { Files, Build, Done, Installed, Uninstalled, Report };
enum class Step { Read, Translate, Compile, Link, Shaders, Count };
constexpr const char* kStepNames[] = {"Read your game files", "Translate the game to PC code", "Compile",
                                      "Put the game together", "Prepare the shaders"};
constexpr float kStepWeight[] = {0.10f, 0.10f, 0.48f, 0.04f, 0.28f};

struct Wizard {
    Page page = Page::Files;
    platform::Display* display = nullptr;
    Texture icon;
    fs::path icon_jpeg; // the game's icon, read from the player's files (LocalAppData only)
    bool icon_loaded = false;

    // game files (found on the PC, browsed, dropped)
    std::mutex mutex;
    std::vector<setup::GameFile> files;
    std::vector<fs::path> pending; // added by the user, waiting to be probed
    std::atomic<bool> scanning{false}, probing{false}, stop{false};
    std::thread scan_thread, probe_thread, icon_thread;
    std::atomic<bool> icon_busy{false};
    std::string icon_key; // what the icon was read from (refresh when it changes)

    fs::path keys;
    std::vector<fs::path> keys_found;
    builder::Toolchain toolchain;
    fs::path clang; // clang-cl: a much faster build and a slightly faster game (empty: cl only)
    std::atomic<bool> toolchain_ready{false};

    fs::path install_dir = setup::DefaultInstallDir();
    fs::path build_dir = setup::DefaultBuildDir();
    fs::path sdk_dir;
    int language = WindowsLanguage();
    int workers = 0;   // 0 = automatic
    int memory_gb = 0; // 0 = automatic
    bool repair = false; // an existing install is being updated / repaired
    int desktop_w = 0, desktop_h = 0; // the main monitor (the game's first window size)

    // build
    builder::Progress build;
    loader::InstallProgress install;
    std::thread worker;
    std::atomic<int> step{0};
    std::atomic<bool> working{false}, failed{false}, cancelled{false};
    std::string error;
    Clock::time_point step_start[int(Step::Count) + 1]{};
    double step_secs[int(Step::Count)]{};
    Clock::time_point started{};
    bool show_details = false;
    bool confirm_cancel = false;
    double built_minutes = 0;
    std::atomic<int> shader_total{0}, shader_left{-1}; // from the shader step's "X of N left" lines in build.log

    // done
    bool want_desktop = true, want_start_menu = true;
    bool finished_shortcuts = false;
    std::string done_note;

    // installed / uninstall
    std::optional<setup::Installed> installed;
    bool open_uninstall = false;
    bool keep_saves = true;
    bool auto_uninstall = false; // (developer test: MUA3_SETUP_DO_UNINSTALL presses Uninstall)
    std::vector<std::string> uninstall_left;

    // report
    char about[4096] = {};
    bool report_save = true;
    report::Result last_report;

    std::string status;

    ~Wizard() {
        stop = true;
        build.cancel = true;
        install.cancel = true;
        for (auto* t : {&scan_thread, &probe_thread, &icon_thread, &worker})
            if (t->joinable())
                t->join();
    }

    // -- files
    void StartScan() {
        scanning = true;
        scan_thread = std::thread([this] {
            keys_found = setup::FindKeys();
            {
                std::lock_guard lk(mutex);
                if (keys.empty() && !keys_found.empty())
                    keys = keys_found.front();
            }
            auto found = setup::FindGameFiles(setup::CandidateFolders(), stop);
            {
                std::lock_guard lk(mutex);
                for (auto& f : found)
                    pending.push_back(f);
            }
            scanning = false;
            toolchain = builder::FindToolchain();
            if (toolchain.ok)
                clang = builder::FindClang(toolchain.vcvars);
            LOG_INFO("Setup", "C++ tools: {}; clang-cl: {}", toolchain.ok ? toolchain.version : toolchain.problem,
                     clang.empty() ? "not found" : clang.string());
            toolchain_ready = true;
        });
    }
    void Add(const fs::path& p) {
        std::lock_guard lk(mutex);
        pending.push_back(p);
    }
    // Probes what is waiting, one at a time on a helper thread (opening a dump reads its headers; the version needs
    // the keys to decrypt the content meta).
    void Pump() {
        if (probing)
            return;
        std::vector<fs::path> todo;
        fs::path k;
        {
            std::lock_guard lk(mutex);
            todo.swap(pending);
            k = keys;
        }
        if (todo.empty())
            return;
        if (probe_thread.joinable())
            probe_thread.join();
        probing = true;
        probe_thread = std::thread([this, todo, k] {
            for (auto& p : todo) {
                if (stop)
                    break;
                std::error_code ec;
                bool known = false;
                {
                    const u64 size = fs::file_size(p, ec);
                    std::lock_guard lk(mutex);
                    for (auto& f : files)
                        known |= fs::equivalent(f.path, p, ec) || (f.path.filename() == p.filename() && f.size == size);
                }
                std::string ext = p.extension().string();
                std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                if (known || (ext != ".nsp" && ext != ".xci"))
                    continue;
                setup::GameFile g = setup::Probe(p, k);
                std::lock_guard lk(mutex);
                // One base game and one update at a time: a newly added one replaces the selection.
                if (g.selected && (g.kind == setup::FileKind::Base || g.kind == setup::FileKind::Update))
                    for (auto& f : files)
                        if (f.kind == g.kind)
                            f.selected = false;
                files.push_back(g);
                std::stable_sort(files.begin(), files.end(), [](const setup::GameFile& a, const setup::GameFile& b) {
                    return int(a.kind) < int(b.kind);
                });
            }
            probing = false;
        });
    }
    // The keys changed: check every file again (versions come from the decrypted content meta).
    void Reprobe() {
        std::lock_guard lk(mutex);
        for (auto& f : files)
            pending.push_back(f.path);
        files.clear();
    }
    std::vector<fs::path> Selected() {
        std::lock_guard lk(mutex);
        std::vector<fs::path> out;
        for (auto& f : files)
            if (f.selected)
                out.push_back(f.path);
        return out;
    }
    // The game's icon from the chosen files (shown here and in the window's title bar before anything is installed).
    void RefreshIcon() {
        if (icon_busy || keys.empty())
            return;
        const auto sel = Selected();
        std::string key = keys.string();
        for (auto& p : sel)
            key += "|" + p.string();
        if (sel.empty() || key == icon_key)
            return;
        icon_key = key;
        if (icon_thread.joinable())
            icon_thread.join();
        icon_busy = true;
        icon_thread = std::thread([this, sel, k = keys] {
            const auto jpeg = setup::ReadGameIcon(sel, k);
            if (!jpeg.empty()) {
                const fs::path out = setup::LocalDataDir() / "icon.jpg";
                std::error_code ec;
                fs::create_directories(out.parent_path(), ec);
                if (common::WriteFile(out, jpeg))
                    icon_jpeg = out, icon_loaded = false;
            }
            icon_busy = false;
        });
    }

    // -- what the build needs
    int Jobs() const {
        int j = workers > 0 ? workers : AutoJobs();
        if (memory_gb > 0)
            j = std::min(j, std::max(1, int(memory_gb / 2.5)));
        return std::max(1, j);
    }
    // Automatic: every logical core (the compilers run at below-normal priority, so the PC stays usable), at most
    // one per 2.5 GB of memory.
    static int AutoJobs() {
        MEMORYSTATUSEX ms{sizeof(ms)};
        GlobalMemoryStatusEx(&ms);
        const int by_ram = std::max(1, int(ms.ullTotalPhys / (2560ull << 20)));
        return std::clamp(std::min(int(std::thread::hardware_concurrency()), by_ram), 1, 64);
    }
    // Minutes for the whole build, from rates measured on a real build (2026-11-11: 4 workers on 4 cores, below
    // normal priority, the PC in use): the game is ~1130 MB of ordinary generated C++, compiled at ~0.55 MB per
    // minute per worker, and ~200 MB in 350 huge functions, optimised at ~0.04 MB per minute per worker, of which
    // only as many run at once as fit in half the free memory (~3.6 GB each). Plus reading the game files (~6 min),
    // translating (~3), linking (~2) and the shaders (~55 min on 4 cores). With clang-cl the game's code compiles
    // about 10x faster and no function is a special case.
    double EstimateMinutes() const {
        constexpr double kNormalMb = 1130, kNormalRate = 0.55, kHugeMb = 200, kHugeRate = 0.04, kHugeGb = 3.6;
        const int jobs = Jobs();
        if (toolchain_ready && !clang.empty())
            return 11.0 + 220.0 / jobs + (kNormalMb + kHugeMb) / (kNormalRate * 10 * jobs);
        MEMORYSTATUSEX ms{sizeof(ms)};
        GlobalMemoryStatusEx(&ms);
        const double half_free_gb = double(ms.ullAvailPhys) / double(1ull << 30) / 2;
        const int huge_at_once = std::clamp(int(half_free_gb / kHugeGb), 1, jobs);
        return 11.0 + 220.0 / jobs + kNormalMb / (kNormalRate * jobs) + kHugeMb / (kHugeRate * huge_at_once);
    }
    // While compiling: the rate seen so far (MB of generated C++ compiled), updated by the build watcher.
    std::atomic<double> compile_left_min{-1};
};

std::string Duration(double minutes) {
    if (minutes < 90)
        return fmt::format("{:.0f} min", std::max(1.0, minutes));
    const int m = int(minutes + 0.5);
    return m % 60 >= 5 ? fmt::format("{} h {} min", m / 60, m % 60 / 5 * 5) : fmt::format("{} h", m / 60);
}

Wizard* g_w = nullptr;
bool g_scroll_bottom = false; // (screenshots: the lower half of a long page)
void StartBuild(Wizard& w);

// Files dropped on the window (SDL delivers them through the event queue the display pumps).
bool DropWatch(void*, SDL_Event* e) {
    if (e->type == SDL_EVENT_DROP_FILE && e->drop.data && g_w) {
        const fs::path p = fs::u8path(e->drop.data);
        std::error_code ec;
        if (fs::is_directory(p, ec)) {
            for (auto& f : fs::directory_iterator(p, ec))
                g_w->Add(f.path());
        } else {
            g_w->Add(p);
        }
    }
    return true;
}

// ---- frame: top bar with the step pills, content, bottom bar
void Background() {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    dl->AddRectFilledMultiColor(vp->Pos, ImVec2(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y), col::bg_top,
                                IM_COL32(38, 40, 48, 255), col::bg_bottom, IM_COL32(33, 34, 40, 255));
    // a faint blue light behind the content
    const ImVec2 c(vp->Pos.x + vp->Size.x * 0.78f, vp->Pos.y + vp->Size.y * 0.30f);
    for (int i = 10; i >= 1; i--)
        dl->AddCircleFilled(c, S(40.0f * i), IM_COL32(61, 107, 240, 3), 48);
}

void TopBar(Wizard& w) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 o = ImGui::GetWindowPos();
    const float wdt = ImGui::GetWindowWidth();
    // icon and name
    ImVec2 p(o.x + S(28), o.y + S(18));
    if (w.icon.srv) {
        dl->AddImageRounded((ImTextureID)w.icon.srv, p, ImVec2(p.x + S(28), p.y + S(28)), ImVec2(0, 0), ImVec2(1, 1),
                            IM_COL32_WHITE, S(6));
        p.x += S(38);
    }
    {
        FontScope f(g_semibold, kText);
        dl->AddText(ImVec2(p.x, p.y + S(4)), col::text, "MUA3 Setup");
    }
    // step pills: Game files > Build > Play
    const char* names[] = {"Game files", "Build", "Play"};
    const int cur = w.page == Page::Files ? 0 : w.page == Page::Build ? 1 : w.page == Page::Done ? 2 : -1;
    if (cur < 0)
        return;
    FontScope f(g_semibold, kText);
    float widths[3], total = 0;
    for (int i = 0; i < 3; i++) {
        widths[i] = ImGui::CalcTextSize(names[i]).x + S(30) + (i < cur ? S(24) : 0);
        total += widths[i];
    }
    total += S(28) * 2 + S(8);
    float x = o.x + (wdt - total) * 0.5f;
    const float y0 = o.y + S(13), y1 = o.y + S(51);
    dl->AddRectFilled(ImVec2(x, y0), ImVec2(x + total, y1), IM_COL32(255, 255, 255, 12), (y1 - y0) * 0.5f);
    dl->AddRect(ImVec2(x, y0), ImVec2(x + total, y1), IM_COL32(255, 255, 255, 22), (y1 - y0) * 0.5f);
    x += S(4);
    for (int i = 0; i < 3; i++) {
        const ImVec2 a(x, y0 + S(4)), b(x + widths[i], y1 - S(4));
        const float th = ImGui::GetFontSize();
        float tx = a.x + S(15);
        if (i == cur) {
            for (int g = 3; g >= 1; g--)
                dl->AddRectFilled(ImVec2(a.x - S(2.f * g), a.y - S(1.5f * g)), ImVec2(b.x + S(2.f * g), b.y + S(2.f * g)),
                                  Alpha(col::accent, 18), (b.y - a.y) * 0.5f + S(2.f * g));
            dl->AddRectFilled(a, b, col::accent, (b.y - a.y) * 0.5f);
        }
        if (i < cur) {
            CheckIcon(dl, ImVec2(tx + S(9), (a.y + b.y) * 0.5f), S(9), col::green);
            tx += S(24);
        }
        dl->AddText(ImVec2(tx, (a.y + b.y - th) * 0.5f), i == cur ? col::text : i < cur ? IM_COL32(200, 202, 208, 255) : col::faint,
                    names[i]);
        x = b.x;
        if (i < 2) {
            dl->AddText(ImVec2(x + S(9), (a.y + b.y - th) * 0.5f), col::faint, ">");
            x += S(28);
        }
    }
}

// ---- page: Game files
void FilesPage(Wizard& w) {
    w.Pump();
    w.RefreshIcon();
    {
        FontScope f(g_semibold, kTitle);
        ImGui::TextUnformatted(w.repair ? "Update or repair your game" : "Build your game");
    }
    Dim(w.repair ? "Add an update or DLC, or build the game again. Your saves and settings stay."
                 : "Pick your game files and where the game goes. Setup builds a native PC version from them, once.");
    ImGui::Dummy(S(1, 6));

    // -- game files
    Label("YOUR GAME FILES");
    std::vector<setup::GameFile> files;
    {
        std::lock_guard lk(w.mutex);
        files = w.files;
    }
    const bool busy = w.scanning || w.probing;
    const float row_h = S(48);
    if (BeginPanel("files", ImVec2(0, 0), ImVec2(8, 6))) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        if (files.empty()) {
            const ImVec2 p = ImGui::GetCursorScreenPos();
            if (busy) {
                Spinner(dl, ImVec2(p.x + S(18), p.y + S(20)), S(8), col::accent);
                ImGui::SetCursorScreenPos(ImVec2(p.x + S(38), p.y + S(10)));
                ImGui::TextUnformatted("Looking for your game files on this PC...");
            } else {
                ImGui::SetCursorScreenPos(ImVec2(p.x + S(10), p.y + S(10)));
                ImGui::TextUnformatted("No game files found yet. Browse for them or drop them on this window.");
            }
            ImGui::Dummy(S(1, 12));
        }
        int toggle = -1;
        for (int i = 0; i < int(files.size()); i++) {
            const auto& f = files[i];
            ImGui::PushID(i);
            const ImVec2 a = ImGui::GetCursorScreenPos();
            const float wd = ImGui::GetContentRegionAvail().x;
            const bool usable = f.verdict != setup::Verdict::WrongGame && f.kind != setup::FileKind::Unknown;
            if (ImGui::InvisibleButton("row", ImVec2(wd, row_h)) && usable)
                toggle = i;
            const bool hovered = ImGui::IsItemHovered();
            if (hovered)
                dl->AddRectFilled(a, ImVec2(a.x + wd, a.y + row_h), IM_COL32(255, 255, 255, 10), S(8));
            // selection mark: a radio for the base game and the update (one each), a check box for DLC
            const ImVec2 c(a.x + S(22), a.y + row_h * 0.5f);
            const bool radio = f.kind != setup::FileKind::Dlc;
            if (radio) {
                dl->AddCircle(c, S(9), f.selected ? col::accent_hi : col::faint, 0, S(1.6f));
                if (f.selected)
                    dl->AddCircleFilled(c, S(5), col::accent_hi);
            } else {
                const ImVec2 r0(c.x - S(8), c.y - S(8)), r1(c.x + S(8), c.y + S(8));
                if (f.selected) {
                    dl->AddRectFilled(r0, r1, col::accent, S(4));
                    dl->PathLineTo(ImVec2(c.x - S(4), c.y));
                    dl->PathLineTo(ImVec2(c.x - S(1), c.y + S(3.5f)));
                    dl->PathLineTo(ImVec2(c.x + S(4.5f), c.y - S(3.5f)));
                    dl->PathStroke(IM_COL32_WHITE, 0, S(1.8f));
                } else {
                    dl->AddRect(r0, r1, col::faint, S(4), 0, S(1.6f));
                }
            }
            {
                FontScope fs_(g_semibold, kText);
                const std::string name = Short(f.path.filename(), 74);
                dl->AddText(ImVec2(a.x + S(44), a.y + S(5)), usable ? col::text : col::dim, name.c_str());
            }
            {
                FontScope fs_(g_regular, kSmall);
                const std::string sub = f.Describe() + "  ·  " + Short(f.path.parent_path(), 60);
                dl->AddText(ImVec2(a.x + S(44), a.y + S(26)), col::dim, sub.c_str());
            }
            const ImU32 bc = f.verdict == setup::Verdict::Supported ? col::green
                             : f.verdict == setup::Verdict::Unknown  ? col::amber
                                                                     : col::red;
            Badge(f.Badge(), bc, ImVec2(a.x + wd - S(12), a.y + row_h * 0.5f));
            if (hovered) {
                std::string tip = f.path.string();
                if (f.verdict == setup::Verdict::WrongVersion)
                    tip += fmt::format("\nThis is v{}. The port needs update {} (v{}).", f.version.value_or(0),
                                       setup::kSupportedUpdateName, setup::kSupportedUpdate);
                else if (f.verdict == setup::Verdict::WrongGame)
                    tip += "\nTitle " + f.title_id + " is not Marvel Ultimate Alliance 3.";
                else if (f.verdict == setup::Verdict::Unknown)
                    tip += "\nThe version is checked once your keys are found.";
                ImGui::SetItemTooltip("%s", tip.c_str());
            }
            ImGui::PopID();
        }
        if (busy && !files.empty()) {
            const ImVec2 p = ImGui::GetCursorScreenPos();
            Spinner(dl, ImVec2(p.x + S(22), p.y + S(12)), S(7), col::accent);
            ImGui::SetCursorScreenPos(ImVec2(p.x + S(44), p.y + S(2)));
            Dim("Looking for more...", false);
        }
        if (toggle >= 0) {
            std::lock_guard lk(w.mutex);
            if (toggle < int(w.files.size())) {
                auto& t = w.files[toggle];
                if (t.kind == setup::FileKind::Dlc) {
                    t.selected = !t.selected;
                } else {
                    const bool was = t.selected;
                    for (auto& f : w.files)
                        if (f.kind == t.kind)
                            f.selected = false;
                    t.selected = !was;
                }
            }
        }
    }
    EndPanel();
    // add more
    {
        Dim("Not listed?", false);
        ImGui::SameLine();
        if (Link("Browse...")) {
            for (auto& f : FileDialog(false, true, L"Choose the base game, the update and DLC",
                                      {{L"Switch games (*.nsp; *.xci)", L"*.nsp;*.xci"}, {L"All files", L"*.*"}},
                                      setup::SetupDir()))
                w.Add(f);
        }
        ImGui::SameLine();
        Dim("·", false);
        ImGui::SameLine();
        if (Link("A folder...")) {
            const auto d = FileDialog(true, false, L"Choose a folder with your game files", {}, setup::SetupDir());
            std::error_code ec;
            if (!d.empty())
                for (auto& e : fs::directory_iterator(d[0], ec))
                    w.Add(e.path());
        }
        ImGui::SameLine();
        Dim("·  or drop the files on this window", false);
    }
    // what is still missing
    bool has_base = false, has_update = false, wrong_update = false;
    int dlc = 0;
    for (auto& f : files) {
        has_base |= f.selected && f.kind == setup::FileKind::Base;
        has_update |= f.selected && f.kind == setup::FileKind::Update && f.verdict == setup::Verdict::Supported;
        wrong_update |= f.kind == setup::FileKind::Update && f.verdict == setup::Verdict::WrongVersion;
        dlc += f.selected && f.kind == setup::FileKind::Dlc;
    }
    ImGui::Dummy(S(1, 4));

    // -- keys and build tools, side by side
    const float half = (ImGui::GetContentRegionAvail().x - S(16)) * 0.5f;
    ImGui::BeginGroup();
    Label("KEYS");
    const float tools_h = S(w.toolchain_ready && w.toolchain.ok && w.clang.empty() ? 104 : 84); // (the row's height)
    if (BeginPanel("keys", ImVec2(half, tools_h))) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const bool ok = !w.keys.empty() && fs::exists(w.keys);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        if (ok)
            CheckIcon(dl, ImVec2(p.x + S(9), p.y + S(10)), S(9), col::green);
        else
            dl->AddCircle(ImVec2(p.x + S(9), p.y + S(10)), S(9), col::amber, 0, S(1.6f));
        ImGui::SetCursorScreenPos(ImVec2(p.x + S(28), p.y));
        ImGui::BeginGroup();
        ImGui::TextUnformatted(ok ? "prod.keys found" : "prod.keys needed");
        Dim(ok ? Short(w.keys.parent_path(), 44) : "From your own Switch (Lockpick_RCM). Read where they are, never copied.", true);
        ImGui::EndGroup();
        ImGui::SameLine(ImGui::GetWindowWidth() - S(100));
        ImGui::SetCursorPosY(S(22));
        if (Button(ok ? "Change" : "Find...", S(84, 36))) {
            const auto f = FileDialog(false, false, L"Choose your prod.keys", {{L"Switch keys (prod.keys)", L"*.keys"}, {L"All files", L"*.*"}},
                                      w.keys.empty() ? setup::SetupDir() : w.keys.parent_path());
            if (!f.empty() && f[0] != w.keys) {
                {
                    std::lock_guard lk(w.mutex);
                    w.keys = f[0];
                }
                w.Reprobe();
            }
        }
    }
    EndPanel();
    ImGui::EndGroup();
    ImGui::SameLine(0, S(16));
    ImGui::BeginGroup();
    Label("C++ BUILD TOOLS");
    if (BeginPanel("tools", ImVec2(half, tools_h))) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const bool ready = w.toolchain_ready;
        const bool ok = ready && w.toolchain.ok;
        if (!ready)
            Spinner(dl, ImVec2(p.x + S(9), p.y + S(10)), S(8), col::accent);
        else if (ok)
            CheckIcon(dl, ImVec2(p.x + S(9), p.y + S(10)), S(9), col::green);
        else
            dl->AddCircle(ImVec2(p.x + S(9), p.y + S(10)), S(9), col::amber, 0, S(1.6f));
        ImGui::SetCursorScreenPos(ImVec2(p.x + S(28), p.y));
        ImGui::BeginGroup();
        ImGui::TextUnformatted(!ready ? "Checking..."
                               : ok   ? ("Microsoft C++ " + w.toolchain.version + (w.clang.empty() ? "" : "  ·  clang-cl")).c_str()
                                      : "Microsoft C++ Build Tools needed");
        if (ok && w.clang.empty())
        {
            ImGui::PushStyleColor(ImGuiCol_Text, V(col::amber));
            ImGui::TextWrapped("%s", "clang-cl not found: slower build, slightly slower game.");
            ImGui::PopStyleColor();
        }
        Dim(ok ? (w.clang.empty() ? "Recommended: add \"C++ Clang Compiler for Windows\" in the Visual Studio Installer."
                                  : "Compiles the game on this PC (clang-cl: fast build, faster game).")
               : ready ? "Free from Microsoft: \"Desktop development with C++\"." : "",
            true);
        ImGui::EndGroup();
        if (ready && !ok) {
            ImGui::SameLine(ImGui::GetWindowWidth() - S(100));
            ImGui::SetCursorPosY(S(22));
            if (Button("Get", S(84, 36)))
                ShellExecuteW(nullptr, L"open", L"https://visualstudio.microsoft.com/downloads/#build-tools-for-visual-studio",
                              nullptr, nullptr, SW_SHOWNORMAL);
        }
    }
    EndPanel();
    ImGui::EndGroup();
    ImGui::Dummy(S(1, 4));

    // -- install folder + language
    const float left = ImGui::GetContentRegionAvail().x - S(236);
    const float row_y = ImGui::GetCursorPosY();
    ImGui::BeginGroup();
    Label("INSTALL THE GAME TO");
    {
        ImGui::BeginDisabled(w.repair);
        std::string s = w.install_dir.string();
        ImGui::SetNextItemWidth(left - S(110));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, S(14, 11));
        char buf[600];
        std::snprintf(buf, sizeof(buf), "%s", s.c_str());
        if (ImGui::InputText("##dest", buf, sizeof(buf)))
            w.install_dir = fs::u8path(buf);
        ImGui::PopStyleVar();
        ImGui::SameLine();
        if (Button("Change", S(96, 40))) {
            const auto d = FileDialog(true, false, L"Choose where to install the game", {}, w.install_dir);
            if (!d.empty())
                w.install_dir = d[0].filename() == "Marvel Ultimate Alliance 3" ? d[0] : d[0] / "Marvel Ultimate Alliance 3";
        }
        ImGui::EndDisabled();
    }
    ImGui::EndGroup();
    ImGui::SameLine(0, S(20));
    ImGui::SetCursorPosY(row_y);
    ImGui::GetCurrentWindow()->DC.CurrLineTextBaseOffset = 0; // (top-aligned columns)
    ImGui::BeginGroup();
    Label("GAME LANGUAGE");
    {
        int sel = 0;
        for (int k = 0; k < int(std::size(kLanguages)); k++)
            if (kLanguages[k].first == w.language)
                sel = k;
        ImGui::SetNextItemWidth(S(216));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, S(14, 11));
        if (ImGui::BeginCombo("##lang", kLanguages[sel].second)) {
            for (int k = 0; k < int(std::size(kLanguages)); k++)
                if (ImGui::Selectable(kLanguages[k].second, k == sel))
                    w.language = kLanguages[k].first;
            ImGui::EndCombo();
        }
        ImGui::PopStyleVar();
    }
    ImGui::EndGroup();
    {
        FontScope f(g_regular, kSmall);
        Dim(fmt::format("Build files: about {} in {} while it builds; {} stays when it is done (the compiled code), so "
                        "updates only compile what changed.",
                        setup::Gb(setup::kBuildFilesBytes), Short(w.build_dir, 52), setup::Gb(setup::kBuildFilesKeptBytes)));
        if (!w.repair && Link("Change the build files folder##build")) {
            const auto d = FileDialog(true, false, L"Choose a folder for the build files", {}, w.build_dir);
            if (!d.empty())
                w.build_dir = d[0] / "MUA3 build files";
        }
    }
    ImGui::Dummy(S(1, 4));

    // -- note + workers / memory
    const float combos_w = S(200) * 2 + S(16);
    const float note_w = ImGui::GetContentRegionAvail().x - combos_w - S(24);
    const float combos_x = ImGui::GetCursorPosX() + note_w + S(24);
    const float combos_y = ImGui::GetCursorPosY();
    ImGui::BeginGroup();
    {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddCircleFilled(ImVec2(p.x + S(8), p.y + S(26)), S(8), Alpha(col::accent, 70));
        {
            FontScope f(g_semibold, kSmall);
            dl->AddText(ImVec2(p.x + S(6), p.y + S(18)), col::text, "i");
        }
        ImGui::SetCursorScreenPos(ImVec2(p.x + S(26), p.y + S(16)));
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + note_w - S(30));
        FontScope f(g_regular, kSmall);
        Dim("Builds run at low priority: your PC stays usable. Nothing of the game is downloaded, everything comes "
            "from your own files.");
        ImGui::PopTextWrapPos();
    }
    ImGui::EndGroup();
    ImGui::SameLine(combos_x);
    ImGui::SetCursorPosY(combos_y);
    ImGui::GetCurrentWindow()->DC.CurrLineTextBaseOffset = 0; // (top-aligned columns)
    ImGui::BeginGroup();
    Label("BUILD WORKERS");
    {
        const int cores = std::max(1, int(std::thread::hardware_concurrency()));
        const std::string preview = w.workers > 0 ? std::to_string(w.workers) : fmt::format("Automatic ({})", Wizard::AutoJobs());
        ImGui::SetNextItemWidth(S(200));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, S(14, 11));
        if (ImGui::BeginCombo("##workers", preview.c_str())) {
            if (ImGui::Selectable(fmt::format("Automatic ({})", Wizard::AutoJobs()).c_str(), w.workers == 0))
                w.workers = 0;
            for (int k = 1; k <= cores; k++)
                if (ImGui::Selectable(std::to_string(k).c_str(), w.workers == k))
                    w.workers = k;
            ImGui::EndCombo();
        }
        ImGui::PopStyleVar();
        ImGui::SetItemTooltip("How many parts compile at once. Fewer leave the PC more responsive and take longer.");
    }
    ImGui::EndGroup();
    ImGui::SameLine(0, S(16));
    ImGui::SetCursorPosY(combos_y);
    ImGui::GetCurrentWindow()->DC.CurrLineTextBaseOffset = 0; // (top-aligned columns)
    ImGui::BeginGroup();
    Label("BUILD MEMORY (GB)");
    {
        MEMORYSTATUSEX ms{sizeof(ms)};
        GlobalMemoryStatusEx(&ms);
        const int total = int(ms.ullTotalPhys >> 30);
        const std::string preview = w.memory_gb > 0 ? std::to_string(w.memory_gb) : "Automatic";
        ImGui::SetNextItemWidth(S(200));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, S(14, 11));
        if (ImGui::BeginCombo("##memory", preview.c_str())) {
            if (ImGui::Selectable("Automatic", w.memory_gb == 0))
                w.memory_gb = 0;
            for (int g : {4, 8, 12, 16, 24, 32, 48, 64})
                if (g < total && ImGui::Selectable(std::to_string(g).c_str(), w.memory_gb == g))
                    w.memory_gb = g;
            ImGui::EndCombo();
        }
        ImGui::PopStyleVar();
        ImGui::SetItemTooltip("The most memory the build may use (about 2.5 GB per worker). This PC has %d GB.", total);
    }
    ImGui::EndGroup();
    ImGui::Dummy(S(1, 8));

    // -- the problems that keep the build button off (shown in the bottom bar)
    std::string why;
    if (!has_base)
        why = busy ? "Looking for your game files..." : "Add the base game.";
    else if (!has_update)
        why = wrong_update ? fmt::format("The update is the wrong version: update {} is needed.", setup::kSupportedUpdateName)
                           : fmt::format("Add update {} (v{}).", setup::kSupportedUpdateName, setup::kSupportedUpdate);
    else if (w.keys.empty() || !fs::exists(w.keys))
        why = "Find your prod.keys.";
    else if (w.toolchain_ready && !w.toolchain.ok)
        why = "Install the Microsoft C++ Build Tools.";
    else if (!w.toolchain_ready)
        why = "Checking the C++ tools...";
    else if (!fs::exists(w.sdk_dir / "tools" / "recomp.exe"))
        why = w.repair ? "Run the setup from the downloaded package to rebuild (its sdk folder is needed)."
                       : "This setup is incomplete (its sdk folder is missing).";
    else if (w.install_dir.empty() || !w.install_dir.is_absolute())
        why = "Choose a folder to install to.";
    (void)dlc;
    w.status = why;
    if (g_scroll_bottom)
        ImGui::SetScrollY(ImGui::GetScrollMaxY());
}

// Bottom bar of the Game files page: the space bar, the time and the button.
void FilesBottom(Wizard& w, std::vector<setup::GameFile> files) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 o = ImGui::GetWindowPos();
    const float wd = ImGui::GetWindowWidth(), ht = ImGui::GetWindowHeight();
    const u64 game = setup::InstallBytes(files);
    const setup::Space dest = setup::DriveSpace(w.install_dir), build = setup::DriveSpace(w.build_dir);
    const bool same = dest.drive == build.drive;
    const u64 need = game + (same ? setup::kBuildFilesBytes : 0);
    const bool fits = dest.free >= need && (same || build.free >= setup::kBuildFilesBytes);
    // bar
    const ImVec2 b0(o.x + S(32), o.y + ht * 0.5f - S(3)), b1(b0.x + S(160), b0.y + S(6));
    dl->AddRectFilled(b0, b1, IM_COL32(255, 255, 255, 25), S(3));
    const float frac = dest.free ? std::clamp(float(double(need) / double(dest.free)), 0.03f, 1.0f) : 1.0f;
    dl->AddRectFilled(b0, ImVec2(b0.x + (b1.x - b0.x) * frac, b1.y), fits ? col::green : col::red, S(3));
    const std::string text = fmt::format("Needs {}  ·  {} has {} free", setup::Gb(need), dest.drive, setup::Gb(dest.free));
    dl->AddText(ImVec2(b1.x + S(14), o.y + (ht - ImGui::GetFontSize()) * 0.5f), fits ? col::dim : col::red, text.c_str());
    // time + button
    std::string why = w.status;
    if (why.empty() && !fits)
        why = "Not enough free space: pick another drive.";
    const ImVec2 btn = S(170, 46);
    ImGui::SetCursorScreenPos(ImVec2(o.x + wd - btn.x - S(32), o.y + (ht - btn.y) * 0.5f));
    if (Button(w.repair ? "Update the game" : "Build the game", btn, true, why.empty())) {
        StartBuild(w);
    }
    {
        const std::string t = why.empty() ? "About " + Duration(w.EstimateMinutes()) : why;
        FontScope f(g_regular, kSmall);
        const ImVec2 sz = ImGui::CalcTextSize(t.c_str());
        dl->AddText(ImVec2(o.x + wd - btn.x - S(52) - sz.x, o.y + (ht - sz.y) * 0.5f), why.empty() ? col::dim : col::amber, t.c_str());
    }
}

// ---- the build (worker thread)
void Log(Wizard& w, const std::string& s) { w.build.Log(s); }

void SetStep(Wizard& w, Step s) {
    const int i = int(s);
    const int prev = w.step.load();
    if (i <= prev)
        return;
    const auto now = Clock::now();
    for (int k = prev; k < i; k++) {
        w.step_secs[k] = std::chrono::duration<double>(now - w.step_start[k]).count();
        w.step_start[k + 1] = now;
    }
    w.step = i;
}

void BuildThread(Wizard& w, std::vector<fs::path> files, fs::path keys) {
    std::error_code ec;
    auto fail = [&](const std::string& why) {
        w.error = why;
        Log(w, "FAILED: " + why);
        w.failed = true;
        w.working = false;
    };
    fs::create_directories(w.install_dir, ec);
    if (ec)
        return fail("Could not create " + w.install_dir.string() + ": " + ec.message());
    const fs::path game = w.install_dir / "game";

    // 1. the game data, from the player's dumps (skipped when the same files are installed already)
    bool reuse = false;
    if (auto prev = setup::LoadInstalled(w.install_dir); prev && loader::IsInstalled(game)) {
        auto a = prev->files, b = files;
        std::sort(a.begin(), a.end()), std::sort(b.begin(), b.end());
        reuse = a == b;
    }
    if (reuse) {
        Log(w, "The game data from these files is installed already: kept.");
    } else {
        w.build.Stage("Installing the game data");
        loader::InstallOptions o;
        o.output_dir = game;
        o.extract_romfs = true;
        o.files = files;
        o.keys_file = keys;
        o.keys_dir = keys.parent_path();
        std::string err;
        std::thread poll([&] { // the installer's stages into the details log
            std::string last;
            while (w.working && !w.failed && w.step == 0) {
                const std::string s = w.install.Stage();
                if (s != last && !s.empty())
                    Log(w, s), last = s;
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }
        });
        const bool ok = loader::InstallGame(o, w.install, err);
        SetStep(w, Step::Translate);
        poll.join();
        if (!ok)
            return fail(w.install.cancel ? "Cancelled." : "Installing the game data failed: " + err);
    }
    SetStep(w, Step::Translate);

    // the game's settings: first install gets PC-friendly defaults; the language follows the choice here
    {
        const fs::path ini = w.install_dir / "mua3.ini";
        core::Config c;
        const bool fresh = !fs::exists(ini);
        c.Load(ini);
        if (fresh) {
            if (w.desktop_w > 0)
                c.width = w.desktop_w, c.height = w.desktop_h;
            c.fullscreen = true; // a PC game starts fullscreen; F11 / the GRAPHICS page change it
            c.save_dir = "save";
            c.game_dir = "game";
        }
        c.language = w.language;
        c.docked = true;
        c.Save(ini);
    }
    // the icon (the executable gets it, and the shortcuts / Apps entry use mua3.ico)
    fs::path ico = w.install_dir / "mua3.ico";
    {
        fs::path jpeg = game / "control" / "icon_AmericanEnglish.dat";
        if (!fs::exists(jpeg))
            for (auto& e : fs::directory_iterator(game / "control", ec))
                if (e.path().filename().string().rfind("icon_", 0) == 0)
                    jpeg = e.path();
        if (!setup::MakeIco(jpeg, ico)) {
            Log(w, "Could not make the icon from " + jpeg.string());
            ico.clear();
        }
    }

    // 2.-5. translate, compile, link, shaders (game_builder), from the install folder (shader_cache\ lands there)
    builder::Paths p;
    p.game_dir = game;
    p.sdk_dir = w.sdk_dir;
    p.work_dir = w.build_dir;
    p.out_exe = w.install_dir / "mua3.exe";
    p.icon = ico;
    p.windowed = true;
    if (const char* pre = std::getenv("MUA3_SETUP_PREBUILT")) // developers only (tests): no translate / compile
        p.prebuilt_code = fs::u8path(pre);
    fs::current_path(w.install_dir, ec);
    std::atomic<bool> watching{true};
    std::thread watch([&] { // the builder's stage -> the wizard's step
        auto compile_start = Clock::time_point{};
        double done_at_start = -1;
        auto last_scan = Clock::time_point{};
        while (watching) {
            const std::string s = w.build.GetStage();
            if (s.rfind("Compiling", 0) == 0 && Clock::now() - last_scan > std::chrono::seconds(15)) {
                // time left from the rate so far: MB of generated C++ compiled (parts compiled in an earlier run
                // do not count towards the rate)
                last_scan = Clock::now();
                double total = 0, done = 0;
                std::error_code e2;
                for (auto& f : fs::directory_iterator(w.build_dir / "gen", e2)) {
                    const std::string n = f.path().filename().string();
                    if (n.rfind("recomp_", 0) != 0 || f.path().extension() != ".cpp")
                        continue;
                    const double mb = double(f.file_size(e2)) / (1 << 20);
                    total += mb;
                    const fs::path o = w.build_dir / "obj" / f.path().filename().replace_extension(".obj");
                    if (fs::exists(o, e2) && !fs::exists(fs::path(o).replace_extension(".log"), e2))
                        done += mb;
                }
                if (done_at_start < 0)
                    done_at_start = done, compile_start = Clock::now();
                const double mins = std::chrono::duration<double>(Clock::now() - compile_start).count() / 60;
                if (mins > 5 && done > done_at_start)
                    w.compile_left_min = (total - done) / ((done - done_at_start) / mins);
            }
            if (w.step > int(Step::Compile))
                w.compile_left_min = -1;
            if (s.rfind("Compiling", 0) == 0)
                SetStep(w, Step::Compile);
            else if (s.rfind("Linking", 0) == 0)
                SetStep(w, Step::Link);
            else if (s.rfind("Preparing", 0) == 0) {
                SetStep(w, Step::Shaders);
                // its progress: the last "Compiling N shaders..." / "  X of N left" line the game wrote to build.log
                std::ifstream log(w.build_dir / "build.log", std::ios::binary);
                log.seekg(0, std::ios::end);
                const std::streamoff size = log.tellg();
                const std::streamoff from = std::max<std::streamoff>(0, size - 65536);
                log.seekg(from);
                std::string tail(size_t(size - from), '\0');
                log.read(tail.data(), tail.size());
                int left = -1, total = 0;
                for (size_t at = 0; at < tail.size();) {
                    size_t end = tail.find('\n', at);
                    if (end == std::string::npos)
                        end = tail.size();
                    const std::string line = tail.substr(at, end - at);
                    int a = 0, b = 0;
                    if (std::sscanf(line.c_str(), " %d of %d left", &a, &b) == 2)
                        left = a, total = b;
                    else if (std::sscanf(line.c_str(), "Compiling %d shaders", &b) == 1)
                        left = b, total = b;
                    at = end + 1;
                }
                if (total > 0)
                    w.shader_total = total, w.shader_left = left;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    });
    const bool ok = builder::BuildGame(p, w.Jobs(), w.build);
    watching = false;
    watch.join();
    if (!ok)
        return fail(w.build.GetError());
    SetStep(w, Step::Count);

    // 6. what setup made, for repair / update / uninstall, and the Apps entry
    setup::Installed in;
    in.dir = w.install_dir;
    in.build_dir = w.build_dir;
    in.source_dir = w.sdk_dir.parent_path();
    in.keys = keys;
    in.files = files;
    in.language = w.language;
    in.desktop = w.want_desktop;
    in.start_menu = w.want_start_menu;
    const fs::path self = setup::SetupDir() / "MUA3Setup.exe";
    if (!fs::equivalent(setup::SetupDir(), w.install_dir, ec)) {
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        fs::copy_file(exe, w.install_dir / "MUA3Setup.exe", fs::copy_options::overwrite_existing, ec);
        if (ec)
            Log(w, "Could not copy the setup into the install folder: " + ec.message());
    }
    setup::SaveInstalled(in);
    setup::RegisterUninstall(in, setup::FolderBytes(w.install_dir));
    w.built_minutes = std::chrono::duration<double>(Clock::now() - w.started).count() / 60.0;
    Log(w, fmt::format("Installed to {} in {:.0f} minutes.", w.install_dir.string(), w.built_minutes));
    (void)self;
    w.working = false;
}

void StartBuild(Wizard& w) {
    if (w.worker.joinable())
        w.worker.join();
    w.build.cancel = false;
    w.install.cancel = false;
    w.install.bytes_done = 0;
    w.install.bytes_total = 0;
    {
        std::lock_guard lk(w.build.mutex);
        w.build.error.clear();
        w.build.log.clear();
        w.build.stage.clear();
    }
    w.build.done = 0;
    w.build.total = 0;
    w.failed = false;
    w.cancelled = false;
    w.error.clear();
    w.step = 0;
    w.started = Clock::now();
    for (auto& t : w.step_start)
        t = w.started;
    for (auto& s : w.step_secs)
        s = 0;
    w.working = true;
    w.page = Page::Build;
    w.show_details = false;
    auto files = w.Selected();
    auto keys = w.keys;
    LOG_INFO("Setup", "build: {} files into {}, build files in {}, {} workers", files.size(), w.install_dir.string(),
             w.build_dir.string(), w.Jobs());
    w.worker = std::thread([&w, files, keys] { BuildThread(w, files, keys); });
}

// ---- page: Build
float Overall(Wizard& w, int step, float* step_frac) {
    const double el = std::chrono::duration<double>(Clock::now() - w.step_start[std::min(step, int(Step::Count) - 1)]).count();
    float f = 0;
    switch (Step(step)) {
    case Step::Read:
        f = w.install.bytes_total ? float(double(w.install.bytes_done) / double(w.install.bytes_total)) : 0.0f;
        break;
    case Step::Translate: f = float(std::min(0.95, el / (8 * 60.0))); break;
    case Step::Compile: f = w.build.total ? float(w.build.done) / float(w.build.total) : 0.0f; break;
    case Step::Link: f = float(std::min(0.95, el / 90.0)); break;
    case Step::Shaders: // the real count once the game reports it, a slow estimate before that
        f = w.shader_total > 0 && w.shader_left >= 0 ? float(w.shader_total - w.shader_left) / float(w.shader_total)
                                                     : float(std::min(0.10, el / (10 * 60.0)));
        break;
    default: f = 1; break;
    }
    if (step_frac)
        *step_frac = f;
    float total = 0;
    for (int k = 0; k < int(Step::Count); k++)
        total += k < step ? kStepWeight[k] : k == step ? kStepWeight[k] * f : 0;
    return std::clamp(total, 0.0f, 1.0f);
}

void Ring(ImDrawList* dl, ImVec2 c, float r, float frac, bool done, bool bad) {
    const float th = S(9);
    dl->AddCircle(c, r, IM_COL32(255, 255, 255, 26), 64, th);
    const ImU32 colr = bad ? col::red : done ? col::green : col::accent;
    if (frac > 0.002f) {
        const float a0 = -kPi * 0.5f;
        dl->PathArcTo(c, r, a0, a0 + kPi * 2 * frac, 96);
        dl->PathStroke(colr, 0, th);
        // round caps
        dl->AddCircleFilled(ImVec2(c.x + r * std::cos(a0), c.y + r * std::sin(a0)), th * 0.5f, colr);
        const float a1 = a0 + kPi * 2 * frac;
        dl->AddCircleFilled(ImVec2(c.x + r * std::cos(a1), c.y + r * std::sin(a1)), th * 0.5f, colr);
    }
}

void BuildPage(Wizard& w) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const int step = w.step;
    const bool done = !w.working && !w.failed && step >= int(Step::Count);
    if (done) {
        w.page = Page::Done;
        return;
    }
    float step_frac = 0;
    const float overall = w.failed ? Overall(w, step, &step_frac) : Overall(w, step, &step_frac);
    // ring + title
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float r = S(52);
    const ImVec2 c(p.x + r + S(8), p.y + r + S(8));
    Ring(dl, c, r, overall, false, w.failed);
    {
        FontScope f(g_semibold, kRing);
        const std::string pct = fmt::format("{}%", int(overall * 100));
        const ImVec2 sz = ImGui::CalcTextSize(pct.c_str());
        dl->AddText(ImVec2(c.x - sz.x * 0.5f, c.y - sz.y * 0.5f), col::text, pct.c_str());
    }
    ImGui::SetCursorScreenPos(ImVec2(p.x + 2 * r + S(44), p.y + S(28)));
    ImGui::BeginGroup();
    {
        FontScope f(g_semibold, kTitle);
        ImGui::TextUnformatted(w.failed ? (w.error == "Cancelled." || w.cancelled ? "Build stopped" : "The build stopped")
                                        : "Building your game");
    }
    if (w.failed)
        Dim(w.cancelled ? "Finished parts are kept: building again carries on where it stopped." : "See below for what went wrong.", false);
    else
        Dim((w.compile_left_min >= 0 ? "About " + Duration(w.compile_left_min + (w.step < int(Step::Shaders) ? 220.0 / w.Jobs() : 0)) + " left"
                                     : "Usually about " + Duration(w.EstimateMinutes())) +
                "  ·  your PC stays usable",
            false);
    ImGui::EndGroup();
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + 2 * r + S(36)));

    const float list_h = ImGui::GetContentRegionAvail().y - S(4);
    if (w.failed && !w.cancelled) {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(229, 83, 75, 28));
        ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(229, 83, 75, 80));
        if (BeginPanel("error", ImVec2(0, 0))) {
            ImGui::PushStyleColor(ImGuiCol_Text, V(IM_COL32(255, 170, 160, 255)));
            ImGui::TextWrapped("%s", w.error.c_str());
            ImGui::PopStyleColor();
        }
        EndPanel();
        ImGui::PopStyleColor(2);
        ImGui::Dummy(S(1, 4));
    }
    if (w.show_details) {
        if (BeginPanel("log", ImVec2(0, ImGui::GetContentRegionAvail().y - S(4)))) {
            FontScope f(g_regular, kSmall);
            for (auto& line : w.build.Tail(300))
                ImGui::TextUnformatted(line.c_str());
            if (w.working && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - S(40))
                ImGui::SetScrollHereY(1.0f);
        }
        EndPanel();
        return;
    }
    if (BeginPanel("steps", ImVec2(0, w.failed && !w.cancelled ? 0 : list_h), ImVec2(24, 14))) {
        ImDrawList* d = ImGui::GetWindowDrawList();
        const float row = S(43);
        for (int k = 0; k < int(Step::Count); k++) {
            const ImVec2 a = ImGui::GetCursorScreenPos();
            const ImVec2 ic(a.x + S(11), a.y + row * 0.5f);
            if (k < int(Step::Count) - 1) // the line down to the next step
                d->AddLine(ImVec2(ic.x, ic.y + S(11)), ImVec2(ic.x, ic.y + row - S(11)), IM_COL32(255, 255, 255, 30), S(1.5f));
            const bool is_done = k < step, cur = k == step;
            if (is_done) {
                CheckIcon(d, ic, S(11), col::green);
            } else if (cur) {
                d->AddCircleFilled(ic, S(11), w.failed ? col::red : col::accent);
                if (!w.failed)
                    Spinner(d, ic, S(15), Alpha(col::accent_hi, 200));
                FontScope f(g_semibold, kSmall);
                const std::string n = std::to_string(k + 1);
                const ImVec2 sz = ImGui::CalcTextSize(n.c_str());
                d->AddText(ImVec2(ic.x - sz.x * 0.5f, ic.y - sz.y * 0.5f), IM_COL32_WHITE, n.c_str());
            } else {
                d->AddCircle(ic, S(11), col::faint, 0, S(1.6f));
            }
            {
                FontScope f(is_done || cur ? g_semibold : g_regular, kText);
                std::string name = kStepNames[k];
                if (cur && Step(k) == Step::Compile && w.build.total > 0)
                    name += fmt::format("  ({} / {})", int(w.build.done), int(w.build.total));
                if (cur && Step(k) == Step::Shaders && w.shader_total > 0 && w.shader_left >= 0)
                    name += fmt::format("  ({} / {})", w.shader_total - w.shader_left, int(w.shader_total));
                if (cur && Step(k) == Step::Read && w.install.bytes_total > 0)
                    name += fmt::format("  ({:.1f} / {:.1f} GB)", w.install.bytes_done / 1073741824.0, w.install.bytes_total / 1073741824.0);
                d->AddText(ImVec2(a.x + S(32), ic.y - ImGui::GetFontSize() * 0.5f), is_done || cur ? col::text : col::faint, name.c_str());
            }
            {
                std::string t = "-";
                if (is_done)
                    t = Clock2(w.step_secs[k]);
                else if (cur)
                    t = Clock2(std::chrono::duration<double>(Clock::now() - w.step_start[k]).count());
                FontScope f(g_semibold, kSmall);
                const ImVec2 sz = ImGui::CalcTextSize(t.c_str());
                const float right = a.x + ImGui::GetContentRegionAvail().x;
                d->AddText(ImVec2(right - sz.x, ic.y - sz.y * 0.5f), cur ? col::text : col::dim, t.c_str());
            }
            ImGui::Dummy(ImVec2(1, row));
        }
    }
    EndPanel();
}

void BuildBottom(Wizard& w) {
    const ImVec2 o = ImGui::GetWindowPos();
    const float wd = ImGui::GetWindowWidth(), ht = ImGui::GetWindowHeight();
    ImGui::SetCursorScreenPos(ImVec2(o.x + S(32), o.y + (ht - ImGui::GetFontSize()) * 0.5f));
    if (Link(w.show_details ? "Hide details" : "Show details"))
        w.show_details = !w.show_details;
    const ImVec2 btn = S(120, 44);
    ImGui::SetCursorScreenPos(ImVec2(o.x + wd - btn.x - S(32), o.y + (ht - btn.y) * 0.5f));
    if (w.failed) {
        ImGui::SetCursorScreenPos(ImVec2(o.x + wd - S(170) - btn.x - S(44), o.y + (ht - btn.y) * 0.5f));
        if (Button("Back", btn)) {
            w.page = Page::Files;
            w.failed = false;
        }
        ImGui::SameLine(0, S(12));
        if (Button("Try again", S(170, 44), true))
            StartBuild(w);
        return;
    }
    if (Button("Cancel", btn))
        w.confirm_cancel = true;
    if (w.confirm_cancel) {
        ImGui::OpenPopup("Stop the build?");
        w.confirm_cancel = false;
    }
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, S(24, 20));
    if (ImGui::BeginPopupModal("Stop the build?", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
        {
            FontScope f(g_semibold, 20);
            ImGui::TextUnformatted("Stop the build?");
        }
        Dim("Finished parts are kept: building again carries on where it stopped.", false);
        ImGui::Dummy(S(1, 8));
        if (Button("Keep building", S(160, 40)))
            ImGui::CloseCurrentPopup();
        ImGui::SameLine();
        if (Button("Stop", S(120, 40), true)) {
            w.cancelled = true;
            w.build.cancel = true;
            w.install.cancel = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar();
}

// ---- page: Done
bool LaunchGame(const fs::path& dir) {
    const fs::path exe = dir / "mua3.exe";
    std::wstring cmd = L"\"" + exe.wstring() + L"\"";
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, dir.c_str(), &si, &pi))
        return false;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

void MakeShortcuts(Wizard& w) {
    if (w.finished_shortcuts)
        return;
    w.finished_shortcuts = true;
    const fs::path exe = w.install_dir / "mua3.exe", ico = w.install_dir / "mua3.ico";
    const fs::path icon = fs::exists(ico) ? ico : exe;
    const std::wstring desc = L"Marvel Ultimate Alliance 3: The Black Order";
    std::error_code ec;
    for (auto [want, lnk] : {std::pair{w.want_desktop, setup::DesktopShortcut()}, std::pair{w.want_start_menu, setup::StartMenuShortcut()}}) {
        if (want) {
            if (!setup::CreateShortcut(lnk, exe, w.install_dir, icon, desc))
                w.done_note = "Could not create " + lnk.string();
        } else {
            fs::remove(lnk, ec);
        }
    }
    if (auto in = setup::LoadInstalled(w.install_dir)) {
        in->desktop = w.want_desktop;
        in->start_menu = w.want_start_menu;
        setup::SaveInstalled(*in);
    }
    LOG_INFO("Setup", "shortcuts: desktop {}, start menu {}", w.want_desktop, w.want_start_menu);
}

bool Toggle(const char* id, bool* v) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const ImVec2 sz = S(42, 24);
    const bool pressed = ImGui::InvisibleButton(id, sz);
    if (pressed)
        *v = !*v;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + sz.x, p.y + sz.y), *v ? col::accent : IM_COL32(255, 255, 255, 40), sz.y * 0.5f);
    const float x = *v ? p.x + sz.x - sz.y * 0.5f : p.x + sz.y * 0.5f;
    dl->AddCircleFilled(ImVec2(x, p.y + sz.y * 0.5f), sz.y * 0.5f - S(3), IM_COL32_WHITE);
    return pressed;
}

void DonePage(Wizard& w) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float r = S(52);
    const ImVec2 c(p.x + r + S(8), p.y + r + S(8));
    if (w.icon.srv) {
        dl->AddImageRounded((ImTextureID)w.icon.srv, ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r), ImVec2(0, 0),
                            ImVec2(1, 1), IM_COL32_WHITE, S(16));
        dl->AddCircleFilled(ImVec2(c.x + r - S(6), c.y + r - S(6)), S(14), IM_COL32(30, 32, 38, 255));
        CheckIcon(dl, ImVec2(c.x + r - S(6), c.y + r - S(6)), S(14), col::green);
    } else {
        Ring(dl, c, r, 1.0f, true, false);
        CheckIcon(dl, c, S(22), col::green);
    }
    ImGui::SetCursorScreenPos(ImVec2(p.x + 2 * r + S(44), p.y + S(28)));
    ImGui::BeginGroup();
    {
        FontScope f(g_semibold, kTitle);
        ImGui::TextUnformatted("Ready to play");
    }
    Dim(w.built_minutes > 0 ? fmt::format("Built in {:.0f} minutes  ·  installed to {}", w.built_minutes, Short(w.install_dir, 50))
                            : "Installed to " + Short(w.install_dir, 60),
        false);
    ImGui::EndGroup();
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + 2 * r + S(36)));

    if (BeginPanel("shortcuts", ImVec2(0, 0), ImVec2(24, 16))) {
        auto row = [&](const char* id, const char* title, const char* sub, bool* v) {
            ImGui::BeginGroup();
            ImGui::TextUnformatted(title);
            Dim(sub, false);
            ImGui::EndGroup();
            ImGui::SameLine(ImGui::GetWindowWidth() - S(24) - S(42));
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + S(8));
            Toggle(id, v);
        };
        row("##desk", "Create a desktop shortcut", "Marvel Ultimate Alliance 3, with the game's icon", &w.want_desktop);
        ImGui::Dummy(S(1, 6));
        ImGui::Separator();
        ImGui::Dummy(S(1, 6));
        row("##start", "Add to the Start menu", "Also listed in Windows Settings > Apps, to update or uninstall", &w.want_start_menu);
    }
    EndPanel();
    ImGui::Dummy(S(1, 6));
    if (BeginPanel("settings", ImVec2(0, 0), ImVec2(24, 14))) {
        ImGui::TextUnformatted("Settings are in the game");
        Dim("Display mode, resolution, render scale, frame rate, VSync, anti-aliasing and more: on the title screen open "
            "SYSTEM > GRAPHICS. Run this setup again (or use Windows Settings > Apps) to update, repair or uninstall.");
    }
    EndPanel();
    if (!w.done_note.empty())
        Dim(w.done_note);
}

void DoneBottom(Wizard& w, bool& quit) {
    const ImVec2 o = ImGui::GetWindowPos();
    const float wd = ImGui::GetWindowWidth(), ht = ImGui::GetWindowHeight();
    const ImVec2 btn = S(150, 46);
    ImGui::SetCursorScreenPos(ImVec2(o.x + wd - btn.x * 2 - S(44), o.y + (ht - btn.y) * 0.5f));
    if (Button("Close", btn)) {
        MakeShortcuts(w);
        quit = true;
    }
    ImGui::SameLine(0, S(12));
    if (Button("Play", btn, true)) {
        MakeShortcuts(w);
        if (LaunchGame(w.install_dir))
            quit = true;
        else
            w.done_note = "Could not start " + (w.install_dir / "mua3.exe").string();
    }
}

// ---- page: already installed (run again): play, update / repair, uninstall
void SelfDeleteLater(const fs::path& dir) {
    // This exe may live in the folder being removed: a hidden cmd removes it (and the folder, when empty) once the
    // setup has closed.
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring cmd = L"cmd.exe /c ping -n 3 127.0.0.1 >nul & del /f /q \"" + std::wstring(exe) + L"\" & rmdir \"" +
                       dir.wstring() + L"\"";
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | DETACHED_PROCESS, nullptr,
                       L"C:\\", &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
}

void UninstallModal(Wizard& w) {
    if (w.open_uninstall) {
        ImGui::OpenPopup("Uninstall");
        w.open_uninstall = false;
    }
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(S(520, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, S(26, 22));
    if (ImGui::BeginPopupModal("Uninstall", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize)) {
        {
            FontScope f(g_semibold, 20);
            ImGui::TextUnformatted("Uninstall Marvel Ultimate Alliance 3?");
        }
        Dim("Removes the game, its data and shaders, the build files, the shortcuts and the Apps entry. Your own game "
            "files (.nsp / .xci) and keys are not touched.");
        ImGui::Dummy(S(1, 6));
        ImGui::Checkbox("Keep my saves and screenshots", &w.keep_saves);
        if (!w.keep_saves)
            Dim("Your saves will be deleted too. This cannot be undone.");
        ImGui::Dummy(S(1, 10));
        if (Button("Cancel", S(130, 42)))
            ImGui::CloseCurrentPopup();
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, V(col::red));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, V(IM_COL32(240, 105, 95, 255)));
        if ((ImGui::Button("Uninstall", S(140, 42)) || w.auto_uninstall) && w.installed) {
            w.auto_uninstall = false;
            fs::path here;
            {
                std::error_code ec;
                fs::current_path(setup::SetupDir().root_path(), ec); // not inside the folder being removed
            }
            w.uninstall_left = setup::Uninstall(*w.installed, w.keep_saves);
            LOG_INFO("Setup", "uninstalled {} ({} left)", w.installed->dir.string(), w.uninstall_left.size());
            std::error_code ec;
            if (fs::equivalent(setup::SetupDir(), w.installed->dir, ec))
                SelfDeleteLater(w.installed->dir);
            else {
                fs::remove(w.installed->dir / "MUA3Setup.exe", ec);
                fs::remove(w.installed->dir, ec); // (only when empty)
            }
            w.page = Page::Uninstalled;
            ImGui::CloseCurrentPopup();
        }
        ImGui::PopStyleColor(2);
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar();
}

void InstalledPage(Wizard& w, bool& quit) {
    const auto& in = *w.installed;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float r = S(44);
    if (w.icon.srv)
        dl->AddImageRounded((ImTextureID)w.icon.srv, p, ImVec2(p.x + 2 * r, p.y + 2 * r), ImVec2(0, 0), ImVec2(1, 1),
                            IM_COL32_WHITE, S(14));
    ImGui::SetCursorScreenPos(ImVec2(p.x + (w.icon.srv ? 2 * r + S(24) : 0), p.y + S(14)));
    ImGui::BeginGroup();
    {
        FontScope f(g_semibold, kTitle);
        ImGui::TextUnformatted("Marvel Ultimate Alliance 3 is installed");
    }
    Dim("In " + Short(in.dir, 70), false);
    ImGui::EndGroup();
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + 2 * r + S(28)));
    const bool exe = fs::exists(in.dir / "mua3.exe");
    auto card = [&](const char* id, const char* title, const char* sub, bool primary) {
        ImGui::PushID(id);
        const ImVec2 a = ImGui::GetCursorScreenPos();
        const float wd = ImGui::GetContentRegionAvail().x;
        const float h = S(72);
        const bool pressed = ImGui::InvisibleButton("card", ImVec2(wd, h));
        const bool hov = ImGui::IsItemHovered();
        dl->AddRectFilled(a, ImVec2(a.x + wd, a.y + h), primary ? Alpha(col::accent, hov ? 90 : 60) : IM_COL32(255, 255, 255, hov ? 20 : 10), S(10));
        dl->AddRect(a, ImVec2(a.x + wd, a.y + h), primary ? Alpha(col::accent_hi, 140) : col::panel_border, S(10));
        {
            FontScope f(g_semibold, 17);
            dl->AddText(ImVec2(a.x + S(22), a.y + S(14)), col::text, title);
        }
        {
            FontScope f(g_regular, kSmall);
            dl->AddText(ImVec2(a.x + S(22), a.y + S(40)), primary ? IM_COL32(210, 220, 255, 255) : col::dim, sub);
        }
        {
            FontScope f(g_semibold, 20);
            dl->AddText(ImVec2(a.x + wd - S(36), a.y + h * 0.5f - S(12)), hov ? col::text : col::dim, ">");
        }
        ImGui::Dummy(S(1, 4));
        ImGui::PopID();
        return pressed;
    };
    if (exe && card("play", "Play", "Start the game. Settings are in the game: SYSTEM > GRAPHICS.", true)) {
        if (LaunchGame(in.dir))
            quit = true;
    }
    if (card("update", "Update or repair", "Add an update or DLC, change the language, or build the game again.", false)) {
        w.repair = true;
        w.install_dir = in.dir;
        if (!fs::exists(w.sdk_dir / "tools" / "recomp.exe") && fs::exists(in.source_dir / "sdk" / "tools" / "recomp.exe"))
            w.sdk_dir = in.source_dir / "sdk";
        if (!in.build_dir.empty())
            w.build_dir = in.build_dir;
        w.language = in.language;
        if (!in.keys.empty() && fs::exists(in.keys))
            w.keys = in.keys;
        w.want_desktop = in.desktop;
        w.want_start_menu = in.start_menu;
        for (auto& f : in.files)
            if (fs::exists(f))
                w.Add(f);
        w.page = Page::Files;
    }
    if (card("uninstall", "Uninstall", "Remove the game from this PC. You can keep your saves.", false))
        w.open_uninstall = true;
    UninstallModal(w);
}

void UninstalledPage(Wizard& w, bool& quit) {
    {
        FontScope f(g_semibold, kTitle);
        ImGui::TextUnformatted("Uninstalled");
    }
    if (w.uninstall_left.empty())
        Dim(w.keep_saves ? "The game has been removed. Your saves and screenshots are still in " +
                               (w.installed ? (w.installed->dir / "save").string() : std::string("the install folder")) + "."
                         : "The game has been removed.");
    else {
        Dim("The game has been removed, except:");
        for (auto& s : w.uninstall_left)
            Dim("  " + s);
    }
    ImGui::Dummy(S(1, 12));
    if (Button("Close", S(140, 44), true))
        quit = true;
}

// ---- page: report a problem (the game opens it after a crash: MUA3Setup.exe --crash-report)
void ReportPage(Wizard& w, bool& quit) {
    {
        FontScope f(g_semibold, kTitle);
        ImGui::TextUnformatted("Report a problem");
    }
    if (auto crash = report::NewestCrash())
        Dim("Newest crash: " + crash->headline);
    ImGui::TextWrapped("What happened? What were you doing (chapter, character, menu, button), and what did you see?");
    ImGui::InputTextMultiline("##about", w.about, sizeof(w.about), ImVec2(-1, S(150)));
    ImGui::Checkbox("Include my save, so the problem can be reached again from the same point", &w.report_save);
    const fs::path save = w.report_save ? fs::path("save") : fs::path();
    const fs::path work = w.installed && !w.installed->build_dir.empty() ? w.installed->build_dir : setup::DefaultBuildDir();
    Dim("Included: your text, system and version info, settings, logs, crash files and the build log. Never your keys or game files.");
    if (Button("Create report", S(180, 42), true)) {
        std::vector<std::string> extra;
        const auto tc = builder::FindToolchain();
        extra.push_back("C++ tools: " + (tc.ok ? "MSVC " + tc.version : tc.problem));
        if (tc.ok) {
            const fs::path cc = builder::FindClang(tc.vcvars);
            extra.push_back("clang-cl: " + (cc.empty() ? std::string("not found") : cc.string()));
        }
        w.last_report = report::Create(w.about, extra, "game", work, save);
    }
    ImGui::SameLine();
    if (Button("Close", S(120, 42)))
        quit = true;
    if (w.last_report.ok) {
        ImGui::TextColored(V(col::green), "Saved: %s", w.last_report.zip.string().c_str());
        if (Link("Show the file"))
            ShellExecuteW(nullptr, L"open", L"explorer.exe", (L"/select,\"" + w.last_report.zip.wstring() + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
        ImGui::SameLine();
        Dim("·", false);
        ImGui::SameLine();
        if (Link("Open GitHub issues"))
            ShellExecuteW(nullptr, L"open", L"https://github.com/KaikoClanworth1/mua3-native/issues/new", nullptr, nullptr, SW_SHOWNORMAL);
    } else if (!w.last_report.error.empty()) {
        ImGui::TextColored(V(col::red), "%s", w.last_report.error.c_str());
    }
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    std::error_code ec;
    fs::create_directories(setup::LocalDataDir(), ec);
    common::SetLogFile(setup::LocalDataDir() / "setup.log");
    common::InstallCrashHandler("setup");
    LOG_INFO("Setup", "MUA3 Setup {} in {}", common::Version(), setup::SetupDir().string());

    static Wizard w;
    g_w = &w;
    w.sdk_dir = setup::SetupDir() / "sdk";
    if (const char* sdk = std::getenv("MUA3_SETUP_SDK")) // developers: a release's sdk folder
        w.sdk_dir = fs::u8path(sdk);
    // command line: --uninstall (Windows Apps), --crash-report (the game, after a crash)
    bool want_uninstall = false, want_report = false;
    {
        int argc = 0;
        wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        for (int i = 1; i < argc; i++) {
            const std::wstring a = argv[i];
            if (a == L"--uninstall")
                want_uninstall = true;
            else if (a == L"--crash-report")
                want_report = true, i++;
        }
        LocalFree(argv);
    }
    w.installed = setup::FindInstalled();
    if (!w.installed) // run from an install folder whose Apps entry is gone
        if (auto here = setup::LoadInstalled(setup::SetupDir()))
            w.installed = here;
    if (want_report) {
        w.page = Page::Report;
        if (w.installed)
            fs::current_path(w.installed->dir, ec);
    } else if (w.installed && fs::exists(w.installed->dir)) {
        w.page = Page::Installed;
        w.open_uninstall = want_uninstall;
    }
    if (const char* pg = std::getenv("MUA3_SETUP_PAGE")) { // screenshots / tests
        const std::string s = pg;
        if (s == "files") w.page = Page::Files;
    }
    if (w.page == Page::Files || w.page == Page::Installed)
        w.StartScan();

    core::Config wcfg;
    wcfg.width = 960;
    wcfg.height = 620;
    wcfg.vsync = true;
    platform::Display display;
    w.display = &display;
    if (!display.Init(wcfg, "MUA3 Setup"))
        return 1;
    SDL_Window* win = display.Window();
    if (const SDL_DisplayMode* d = SDL_GetDesktopDisplayMode(SDL_GetPrimaryDisplay()))
        w.desktop_w = d->w, w.desktop_h = d->h;
    g_hwnd = (HWND)SDL_GetPointerProperty(SDL_GetWindowProperties(win), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
    const float content = SDL_GetDisplayContentScale(SDL_GetDisplayForWindow(win));
    g_ui = std::clamp(content > 0 ? content : 1.0f, 1.0f, 3.0f);
    if (const char* ui = std::getenv("MUA3_SETUP_UI"))
        g_ui = std::clamp(float(std::atof(ui)), 1.0f, 3.0f);
    {
        SDL_Rect usable{};
        SDL_GetDisplayUsableBounds(SDL_GetDisplayForWindow(win), &usable);
        int ww = int(S(960)), wh = int(S(620));
        if (usable.w > 0)
            ww = std::min(ww, usable.w - 40), wh = std::min(wh, usable.h - 60);
        SDL_SetWindowSize(win, ww, wh);
        SDL_SetWindowMinimumSize(win, int(S(860)), int(S(560)));
        SDL_SetWindowPosition(win, SDL_WINDOWPOS_CENTERED_DISPLAY(SDL_GetDisplayForWindow(win)),
                              SDL_WINDOWPOS_CENTERED_DISPLAY(SDL_GetDisplayForWindow(win)));
    }
    ApplyTheme();
    LoadFonts();
    ImGui::GetIO().IniFilename = nullptr;
    SDL_AddEventWatch(DropWatch, nullptr);
    if (w.installed) {
        const fs::path jpeg = w.installed->dir / "game" / "control" / "icon_AmericanEnglish.dat";
        if (fs::exists(jpeg))
            w.icon_jpeg = jpeg;
    } else if (fs::exists(setup::LocalDataDir() / "icon.jpg")) {
        w.icon_jpeg = setup::LocalDataDir() / "icon.jpg";
    }

    bool quit = false;
    display.draw_overlay = [&] {
        if (!w.icon_loaded && !w.icon_busy && !w.icon_jpeg.empty()) {
            w.icon_loaded = true;
            if (w.icon.srv)
                w.icon.srv->Release(), w.icon.srv = nullptr;
            w.icon = LoadImageTexture(display.Device(), w.icon_jpeg);
            display.SetIcon(w.icon_jpeg);
        }
        Background();
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->Pos);
        ImGui::SetNextWindowSize(vp->Size);
        ImGui::Begin("setup", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus |
                                           ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar);
        const float top = S(64), bottom = S(78);
        ImGui::BeginChild("top", ImVec2(0, top), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
        TopBar(w);
        ImGui::EndChild();
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, S(64, 14));
        const bool scrolls = w.page == Page::Files || w.page == Page::Report;
        ImGui::BeginChild("content", ImVec2(0, vp->Size.y - top - bottom), ImGuiChildFlags_AlwaysUseWindowPadding,
                          ImGuiWindowFlags_NoBackground | (scrolls ? 0 : ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse));
        ImGui::PopStyleVar();
        static Page shown = w.page;
        if (shown != w.page) { // each page starts at its top
            shown = w.page;
            ImGui::SetScrollY(0);
        }
        if (!scrolls)
            ImGui::SetScrollY(0);
        switch (w.page) {
        case Page::Files: FilesPage(w); break;
        case Page::Build: BuildPage(w); break;
        case Page::Done: DonePage(w); break;
        case Page::Installed: InstalledPage(w, quit); break;
        case Page::Uninstalled: UninstalledPage(w, quit); break;
        case Page::Report: ReportPage(w, quit); break;
        }
        ImGui::EndChild();
        ImGui::BeginChild("bottom", ImVec2(0, bottom), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
        switch (w.page) {
        case Page::Files: {
            std::vector<setup::GameFile> files;
            {
                std::lock_guard lk(w.mutex);
                files = w.files;
            }
            FilesBottom(w, files);
            break;
        }
        case Page::Build: BuildBottom(w); break;
        case Page::Done: DoneBottom(w, quit); break;
        default: break;
        }
        ImGui::EndChild();
        ImGui::End();
    };

    // Automation for tests and screenshots (window hidden with MUA3_HEADLESS):
    //   MUA3_SETUP_SHOT=<dir>   saves page screenshots there (files.bmp, build_*.bmp, done.bmp, ...)
    //   MUA3_SETUP_AUTO=<dir>   installs into <dir> with what was found, makes the shortcuts, and exits (exit code 0
    //                           when the game was built); MUA3_SETUP_SANDBOX keeps shortcuts / Apps entry out of the
    //                           real desktop.
    const char* shot_dir = std::getenv("MUA3_SETUP_SHOT");
    const char* auto_dir = std::getenv("MUA3_SETUP_AUTO");
    const int auto_jobs = std::getenv("MUA3_SETUP_JOBS") ? std::atoi(std::getenv("MUA3_SETUP_JOBS")) : 0;
    auto shot = [&](const char* name) {
        if (!shot_dir)
            return;
        fs::create_directories(fs::u8path(shot_dir), ec);
        display.capture_path = fs::u8path(shot_dir) / name;
        LOG_INFO("Setup", "screenshot {}", name);
    };
    int exit_code = 0;
    Page last_page = w.page;
    int page_frames = 0, last_shot_step = -1;
    double next_build_shot = 0;
    int details_close = -1;
    bool auto_started = false;
    for (int frame = 0; !quit && display.PumpEvents(); frame++) {
        if (w.page != last_page)
            last_page = w.page, page_frames = 0;
        page_frames++;
        if (shot_dir || auto_dir) {
            const bool settled = !w.scanning && !w.probing && !w.icon_busy && w.toolchain_ready;
            if (w.page == Page::Files && settled && page_frames > 30) {
                if (auto_dir && !auto_started) {
                    if (page_frames == 31) {
                        w.install_dir = fs::u8path(auto_dir);
                        if (auto_jobs > 0)
                            w.workers = auto_jobs;
                    }
                    if (page_frames == 40)
                        shot("1_game_files.bmp");
                    if (page_frames == 44)
                        g_scroll_bottom = true;
                    if (page_frames == 48)
                        shot("1_game_files_lower.bmp"), g_scroll_bottom = false;
                    if (page_frames == 55) {
                        if (w.status.empty()) {
                            auto_started = true;
                            StartBuild(w);
                        } else {
                            LOG_ERROR("Setup", "auto: cannot build: {}", w.status);
                            exit_code = 2;
                            break;
                        }
                    }
                } else if (!auto_dir) {
                    if (page_frames == 40)
                        shot("1_game_files.bmp");
                    if (page_frames == 44)
                        g_scroll_bottom = true;
                    if (page_frames == 48)
                        shot("1_game_files_lower.bmp");
                    if (page_frames == 52)
                        break;
                }
            }
            if (w.page == Page::Build) {
                const double now = ImGui::GetTime();
                if (w.step != last_shot_step && page_frames > 5) { // one picture per step
                    last_shot_step = w.step;
                    shot(fmt::format("2_build_step{}.bmp", int(w.step) + 1).c_str());
                    next_build_shot = now + 60;
                } else if (now > next_build_shot && w.show_details == false && w.step == int(Step::Compile)) {
                    w.show_details = true; // and one of the details log
                    next_build_shot = now + 1e9;
                } else if (w.show_details && page_frames % 120 == 0) {
                    shot("2_build_details.bmp"); // (taken at the end of this frame: the log closes a few frames later)
                    details_close = page_frames + 5;
                } else if (w.show_details && page_frames == details_close) {
                    w.show_details = false;
                }
                if (w.failed && auto_dir) {
                    shot("2_build_failed.bmp");
                    exit_code = 3;
                    LOG_ERROR("Setup", "auto: build failed: {}", w.error);
                }
            }
            if (w.page == Page::Done && page_frames == 20)
                shot("3_done.bmp");
            if (w.page == Page::Done && page_frames == 25 && auto_dir) {
                MakeShortcuts(w);
                break;
            }
            if (w.page == Page::Installed && page_frames == 40) {
                shot("4_installed.bmp");
            }
            if (w.page == Page::Installed && page_frames == 45 &&
                (std::getenv("MUA3_SETUP_UNINSTALL_SHOT") || std::getenv("MUA3_SETUP_DO_UNINSTALL")))
                w.open_uninstall = true;
            if (w.page == Page::Installed && page_frames == 60 && std::getenv("MUA3_SETUP_UNINSTALL_SHOT"))
                shot("5_uninstall.bmp");
            if (w.page == Page::Installed && page_frames == 70 && std::getenv("MUA3_SETUP_DO_UNINSTALL"))
                w.auto_uninstall = true;
            else if (w.page == Page::Installed && page_frames == 65 && !auto_dir && !std::getenv("MUA3_SETUP_DO_UNINSTALL"))
                break;
            if (w.page == Page::Uninstalled && page_frames == 20)
                shot("6_uninstalled.bmp");
            if (w.page == Page::Uninstalled && page_frames == 25 && std::getenv("MUA3_SETUP_DO_UNINSTALL"))
                break;
            if (exit_code == 3 && page_frames > 3)
                break;
        }
        display.BeginFrame();
        display.EndFrame();
        display.PaceFrame();
    }
    if (w.page == Page::Done)
        MakeShortcuts(w);
    // a build still running when the window closes stops (finished parts are kept)
    w.build.cancel = true;
    w.install.cancel = true;
    if (w.worker.joinable())
        w.worker.join();
    SDL_RemoveEventWatch(DropWatch, nullptr);
    display.Shutdown();
    return exit_code;
}
