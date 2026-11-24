// The launcher (see launcher.h): the main menu, PLAY, MODS and SETTINGS, drawn in the style of the game's menus.
#define IMGUI_DEFINE_MATH_OPERATORS
#include "setup/launcher.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <map>
#include <mutex>
#include <thread>

#include <windows.h>
#include <shellapi.h>

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_internal.h> // (ImDrawList::_Data: the white pixel for gradients)

#include "core/config.h"
#include "core/hle/mods.h"
#include "launcher/report.h"
#include "setup/diagnostics.h"
#include "setup/ui_util.h"
#include "setup/updater.h"

namespace launcher {
namespace {

using Clock = std::chrono::steady_clock;

// ---- look: the game's menus (a blue starfield, angled panels, yellow highlight bars, bold italic caps)
namespace col {
const ImU32 yellow = IM_COL32(255, 214, 10, 255), yellow2 = IM_COL32(242, 183, 5, 255), ink = IM_COL32(5, 7, 15, 255);
const ImU32 text = IM_COL32(233, 239, 255, 255), dim = IM_COL32(157, 182, 230, 255), faint = IM_COL32(110, 126, 160, 255);
const ImU32 card = IM_COL32(14, 24, 52, 225), card2 = IM_COL32(6, 10, 24, 235), line = IM_COL32(255, 255, 255, 22);
const ImU32 green = IM_COL32(126, 224, 164, 255), amber = IM_COL32(255, 176, 64, 255), red = IM_COL32(255, 110, 96, 255);
} // namespace col
ImU32 Alpha(ImU32 c, int a) { return (c & 0x00FFFFFF) | (ImU32(std::clamp(a, 0, 255)) << 24); }

ImFont* g_head = nullptr; // Exo 2 ExtraBold Italic
ImFont* g_body = nullptr; // Exo 2 Regular
float g_u = 1;            // layout unit: the window as 1280 x 720
float U(float v) { return v * g_u; }

struct Font {
    Font(ImFont* f, float size) { ImGui::PushFont(f, U(size)); }
    ~Font() { ImGui::PopFont(); }
};

// A quad with a colour per corner (gradients on angled shapes).
void Quad(ImDrawList* dl, ImVec2 a, ImVec2 b, ImVec2 c, ImVec2 d, ImU32 ca, ImU32 cb, ImU32 cc, ImU32 cd) {
    const ImVec2 uv = dl->_Data->TexUvWhitePixel;
    dl->PrimReserve(6, 4);
    const ImDrawIdx i = ImDrawIdx(dl->_VtxCurrentIdx);
    dl->PrimWriteIdx(i), dl->PrimWriteIdx(ImDrawIdx(i + 1)), dl->PrimWriteIdx(ImDrawIdx(i + 2));
    dl->PrimWriteIdx(i), dl->PrimWriteIdx(ImDrawIdx(i + 2)), dl->PrimWriteIdx(ImDrawIdx(i + 3));
    dl->PrimWriteVtx(a, uv, ca), dl->PrimWriteVtx(b, uv, cb), dl->PrimWriteVtx(c, uv, cc), dl->PrimWriteVtx(d, uv, cd);
}
// A slanted bar (the game's highlight shape): left edge straight, right edge leaning by `lean`.
void Bar(ImDrawList* dl, ImVec2 a, ImVec2 b, float lean, ImU32 c0, ImU32 c1) {
    Quad(dl, a, ImVec2(b.x, a.y), ImVec2(b.x - lean, b.y), ImVec2(a.x, b.y), c0, c1, c1, c0);
}
// A parallelogram (both edges leaning).
void Slant(ImDrawList* dl, ImVec2 a, ImVec2 b, float lean, ImU32 c0, ImU32 c1) {
    Quad(dl, ImVec2(a.x + lean, a.y), ImVec2(b.x, a.y), ImVec2(b.x - lean, b.y), ImVec2(a.x, b.y), c0, c1, c1, c0);
}
// A soft round glow: `inner` at the centre fading to nothing at radius r.
void Glow(ImDrawList* dl, ImVec2 c, float r, ImU32 inner) {
    const ImVec2 uv = dl->_Data->TexUvWhitePixel;
    const int n = 64;
    dl->PrimReserve(n * 3, n + 1);
    const ImDrawIdx base = ImDrawIdx(dl->_VtxCurrentIdx);
    for (int i = 0; i < n; i++)
        dl->PrimWriteIdx(base), dl->PrimWriteIdx(ImDrawIdx(base + 1 + i)), dl->PrimWriteIdx(ImDrawIdx(base + 1 + (i + 1) % n));
    dl->PrimWriteVtx(c, uv, inner);
    for (int i = 0; i < n; i++) {
        const float t = 6.2831853f * i / n;
        dl->PrimWriteVtx(ImVec2(c.x + std::cos(t) * r, c.y + std::sin(t) * r), uv, inner & 0x00FFFFFF);
    }
}
// Text with extra spacing between letters (the game's small caps labels). UTF-8: one character at a time.
template <typename F>
void EachChar(const char* s, F&& f) {
    while (*s) {
        unsigned int c = 0;
        const int n = ImTextCharFromUtf8(&c, s, nullptr);
        f(s, s + std::max(1, n));
        s += std::max(1, n);
    }
}
void Spaced(ImDrawList* dl, ImVec2 p, ImU32 c, const char* s, float spacing) {
    EachChar(s, [&](const char* a, const char* b) {
        dl->AddText(p, c, a, b);
        p.x += ImGui::CalcTextSize(a, b).x + spacing;
    });
}
float SpacedWidth(const char* s, float spacing) {
    float w = 0;
    EachChar(s, [&](const char* a, const char* b) { w += ImGui::CalcTextSize(a, b).x + spacing; });
    return w;
}
void Shadowed(ImDrawList* dl, ImVec2 p, ImU32 c, const char* s) {
    dl->AddText(ImVec2(p.x + U(2), p.y + U(3)), IM_COL32(0, 0, 0, 120), s);
    dl->AddText(p, c, s);
}
// The game's button prompt: a dark circle with the button letter, then the action.
float Prompt(ImDrawList* dl, float right, float y, const char* button, const char* label) {
    Font f(g_body, 17);
    const float lw = SpacedWidth(label, U(1.2f));
    const float r = U(14);
    const float x = right - lw;
    Spaced(dl, ImVec2(x, y - ImGui::GetFontSize() * 0.5f), col::text, label, U(1.2f));
    const ImVec2 c(x - U(10) - r, y);
    dl->AddCircleFilled(c, r, IM_COL32(58, 63, 74, 255));
    Font b(g_head, 15);
    const ImVec2 ts = ImGui::CalcTextSize(button);
    dl->AddText(ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f), col::yellow, button);
    return c.x - r - U(26);
}

bool Pressed(std::initializer_list<ImGuiKey> keys, bool repeat = true) {
    for (ImGuiKey k : keys)
        if (ImGui::IsKeyPressed(k, repeat))
            return true;
    return false;
}
bool NavUp() { return Pressed({ImGuiKey_UpArrow, ImGuiKey_GamepadDpadUp, ImGuiKey_GamepadLStickUp}); }
bool NavDown() { return Pressed({ImGuiKey_DownArrow, ImGuiKey_GamepadDpadDown, ImGuiKey_GamepadLStickDown}); }
bool NavAccept() { return Pressed({ImGuiKey_Enter, ImGuiKey_KeypadEnter, ImGuiKey_GamepadFaceDown}, false); }
bool NavBack() { return Pressed({ImGuiKey_Escape, ImGuiKey_Backspace, ImGuiKey_GamepadFaceRight}, false); }

// ---- stars (fixed, seeded)
struct Star {
    float x, y, r, a, phase;
};
std::vector<Star> g_stars;
void MakeStars() {
    u32 s = 7;
    auto rnd = [&] { return float((s = s * 1664525u + 1013904223u) >> 8) / float(1u << 24); };
    g_stars.clear();
    for (int i = 0; i < 520; i++) {
        Star st{rnd(), rnd(), 0, rnd(), rnd() * 6.283f};
        st.r = rnd() < 0.06f ? 1.5f : 0.75f;
        g_stars.push_back(st);
    }
}

// ---- state
enum Section { kPlay, kMods, kSettings, kExit, kSections };
const char* const kSectionNames[kSections] = {"PLAY", "MODS", "SETTINGS", "EXIT"};

struct ModRow {
    hle::mods::Mod mod;
    // file counts (filled by the analysis thread)
    bool counted = false;
    int files = 0, replaced = 0, added = 0, lost = 0;
    std::string lost_to; // the mods that win this mod's files
    fs::path preview;
    ui::Texture tex;
    bool tex_tried = false;
};

struct State {
    Host* host = nullptr;
    fs::path dir;
    int sel = kPlay;        // the highlighted menu entry (its section shows on the right)
    bool in_section = false; // keyboard / controller inside the section (else on the menu)
    float t = 0;

    // PLAY
    std::string game_version, dlc;
    std::atomic<bool> probed{false};
    std::thread probe;
    HANDLE game = nullptr;
    FILETIME launched{};
    std::string ended; // how the game ended last time (empty: nothing to say)
    bool crashed = false;

    // MODS
    std::vector<ModRow> mods;
    bool mods_on = true;
    int mod_sel = 0;
    std::string mods_note;
    ImU32 mods_note_col = col::dim;
    std::mutex mutex; // the analysis and the install thread
    std::thread analysis;
    std::atomic<int> generation{0};
    std::thread install;
    std::atomic<bool> installing{false};
    std::string install_name, install_result, installed_mod;
    bool install_done = false;
    int confirm_remove = -1;

    // SETTINGS
    core::Config cfg;
    int settings_sel = 0;

    // SETTINGS > RUN DIAGNOSTICS
    diag::Runner diag;
    bool diag_open = false, diag_shown_result = false;
    std::vector<bool> diag_pick = {true, false, false};
    int diag_sel = 0;

    // a newer version of the port (checked once when the launcher opens)
    updater::Updater update;
    bool update_started = false; // the player asked for it (its failures show)
    // the SETTINGS rows last drawn (for test logs)
    int settings_rows = 0;
    const char* settings_labels[4]{};
} g;

// RecompLauncher installs (setup --prepare in the install folder) are updated by RecompLauncher.
bool ManagedElsewhere() {
    std::error_code ec;
    const setup::Installed& in = g.host->installed;
    return !in.source_dir.empty() && fs::equivalent(in.source_dir, in.dir, ec);
}

// The update banner on PLAY: what is new and the button (height 0 when there is nothing to say).
float UpdateBannerHeight() {
    using S = updater::Updater::State;
    const S st = g.update.state;
    return st == S::Available || st == S::Downloading || st == S::Unpacking || st == S::Ready ||
                   (st == S::Failed && g.update_started)
               ? U(74)
               : 0;
}

fs::path ModsDir() { return g.dir / "mods"; }
fs::path Ini() { return g.dir / "mua3.ini"; }

// ---- PLAY: what is installed
void ProbeGame() {
    if (g.probe.joinable())
        g.probe.join();
    g.probed = false;
    g.probe = std::thread([files = g.host->installed.files, keys = g.host->installed.keys] {
        std::string version = "Base game only", dlc;
        int dlcs = 0;
        for (auto& f : files) {
            std::error_code ec;
            if (!fs::exists(f, ec))
                continue;
            setup::GameFile gf = setup::Probe(f, keys);
            if (gf.kind == setup::FileKind::Update)
                version = gf.version && *gf.version == setup::kSupportedUpdate
                              ? std::string(setup::kSupportedUpdateName) + " (supported)"
                              : (gf.version ? fmt::format("update v{}", *gf.version) : std::string("update"));
            else if (gf.kind == setup::FileKind::Dlc)
                dlcs++;
        }
        dlc = dlcs == 0 ? "None" : dlcs == 1 ? "1 pack" : fmt::format("{} packs", dlcs);
        std::lock_guard lk(g.mutex);
        g.game_version = version;
        g.dlc = dlc;
        g.probed = true;
    });
}

bool Launch() {
    const fs::path exe = g.dir / "mua3.exe";
    std::wstring cmd = L"\"" + exe.wstring() + L"\"";
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, g.dir.c_str(), &si, &pi)) {
        g.ended = "The game could not be started.";
        g.crashed = true;
        return false;
    }
    CloseHandle(pi.hThread);
    g.game = pi.hProcess;
    GetSystemTimeAsFileTime(&g.launched);
    g.ended.clear();
    g.crashed = false;
    LOG_INFO("Launcher", "game started (pid {})", pi.dwProcessId);
    if (g.host->window)
        SDL_MinimizeWindow(g.host->window);
    return true;
}

void CheckGame() {
    if (!g.game || WaitForSingleObject(g.game, 0) != WAIT_OBJECT_0)
        return;
    DWORD code = 0;
    GetExitCodeProcess(g.game, &code);
    CloseHandle(g.game);
    g.game = nullptr;
    LOG_INFO("Launcher", "game ended (code {})", code);
    const auto crash = report::NewestCrash();
    const u64 since = (u64(g.launched.dwHighDateTime) << 32) | g.launched.dwLowDateTime;
    if (crash && crash->time >= since) {
        g.crashed = true;
        g.ended = crash->headline;
    } else if (code != 0) {
        g.crashed = true;
        g.ended = fmt::format("The game closed with code {}.", code);
    }
    if (g.host->window) {
        SDL_RestoreWindow(g.host->window);
        SDL_RaiseWindow(g.host->window);
    }
    // a mod or setting the game itself changed (its MODS / options pages)
    g.cfg.Load(Ini());
}

// ---- MODS
std::string Lower(std::string s) {
    for (char& c : s)
        c = char(std::tolower(u8(c)));
    return s;
}

fs::path FindPreview(const hle::mods::Mod& m) {
    static const char* const kNames[] = {"preview", "cover", "thumbnail", "thumb", "screenshot", "icon"};
    std::error_code ec;
    fs::path any;
    for (const fs::path& base : {m.folder, m.romfs.empty() ? m.folder : m.romfs.parent_path()}) {
        for (auto& e : fs::directory_iterator(base, ec)) {
            if (!e.is_regular_file(ec))
                continue;
            const std::string ext = Lower(e.path().extension().string());
            if (ext != ".png" && ext != ".jpg" && ext != ".jpeg" && ext != ".bmp")
                continue;
            const std::string stem = Lower(e.path().stem().string());
            for (const char* n : kNames)
                if (stem.rfind(n, 0) == 0)
                    return e.path();
            if (any.empty())
                any = e.path();
        }
    }
    return any;
}

// Counts each mod's files, which of them replace a game file, and which a later mod replaces again.
void Analyze() {
    const int gen = ++g.generation;
    std::vector<hle::mods::Mod> mods;
    for (auto& r : g.mods)
        mods.push_back(r.mod);
    const fs::path game_romfs = g.dir / g.cfg.game_dir / "romfs";
    if (g.analysis.joinable())
        g.analysis.join();
    g.analysis = std::thread([mods, game_romfs, gen] {
        struct Count {
            int files = 0, replaced = 0, added = 0, lost = 0;
            std::vector<std::string> lost_to;
        };
        std::vector<Count> out(mods.size());
        std::map<std::string, size_t> owner; // file -> the mod that wins it (so far)
        for (size_t i = 0; i < mods.size(); i++) {
            if (mods[i].romfs.empty())
                continue;
            std::error_code ec;
            for (auto it = fs::recursive_directory_iterator(mods[i].romfs, ec); !ec && it != fs::recursive_directory_iterator();
                 it.increment(ec)) {
                if (!it->is_regular_file(ec))
                    continue;
                const fs::path rel = fs::relative(it->path(), mods[i].romfs, ec);
                out[i].files++;
                (fs::exists(game_romfs / rel, ec) ? out[i].replaced : out[i].added)++;
                if (!mods[i].enabled)
                    continue;
                const std::string key = Lower(rel.generic_string());
                if (auto o = owner.find(key); o != owner.end()) {
                    out[o->second].lost++;
                    auto& names = out[o->second].lost_to;
                    if (std::find(names.begin(), names.end(), mods[i].name) == names.end())
                        names.push_back(mods[i].name);
                    o->second = i;
                } else {
                    owner[key] = i;
                }
            }
        }
        std::lock_guard lk(g.mutex);
        if (gen != g.generation)
            return;
        for (size_t i = 0; i < mods.size() && i < g.mods.size(); i++) {
            ModRow& r = g.mods[i];
            r.files = out[i].files, r.replaced = out[i].replaced, r.added = out[i].added, r.lost = out[i].lost;
            r.lost_to.clear();
            for (auto& n : out[i].lost_to)
                r.lost_to += (r.lost_to.empty() ? "" : ", ") + n;
            r.counted = true;
        }
    });
}

void ScanMods() {
    std::vector<hle::mods::Mod> found = hle::mods::Scan(false, ModsDir());
    {
        std::lock_guard lk(g.mutex);
        for (auto& r : g.mods)
            ui::Release(r.tex);
        g.mods.clear();
        for (auto& m : found) {
            ModRow r;
            r.mod = m;
            r.preview = FindPreview(m);
            g.mods.push_back(std::move(r));
        }
        g.mod_sel = std::clamp(g.mod_sel, 0, std::max(0, int(g.mods.size()) - 1));
    }
    Analyze();
}

// Puts the keyboard row on the mod just installed (and scrolls to it).
void SelectInstalled() {
    std::lock_guard lk(g.mutex);
    for (int i = 0; i < int(g.mods.size()); i++)
        if (g.mods[i].mod.name == g.installed_mod)
            g.mod_sel = i, g.in_section = true;
    g.installed_mod.clear();
}

void SaveMods(const char* what) {
    std::vector<hle::mods::Mod> list;
    for (auto& r : g.mods)
        list.push_back(r.mod);
    hle::mods::EnsureFolder(ModsDir());
    const bool ok = hle::mods::SaveOrder(list, ModsDir());
    core::Config c;
    c.Load(Ini());
    c.mods_enabled = g.mods_on;
    g.cfg.mods_enabled = g.mods_on;
    const bool ini_ok = c.Save(Ini());
    g.mods_note = ok && ini_ok ? fmt::format("{} Applies the next time the game starts.", what)
                               : std::string("Could not save the mod list (is the folder read-only?).");
    g.mods_note_col = ok && ini_ok ? col::green : col::red;
    Analyze();
}

void MoveMod(int i, int d) {
    const int j = i + d;
    if (i < 0 || j < 0 || i >= int(g.mods.size()) || j >= int(g.mods.size()))
        return;
    {
        std::lock_guard lk(g.mutex);
        std::swap(g.mods[i], g.mods[j]);
    }
    g.mod_sel = j;
    SaveMods(fmt::format("Moved {} {}.", g.mods[j].mod.name, d < 0 ? "up" : "down").c_str());
}

void ToggleMod(int i) {
    if (i < 0 || i >= int(g.mods.size()))
        return;
    g.mods[i].mod.enabled = !g.mods[i].mod.enabled;
    SaveMods(fmt::format("{} is {}.", g.mods[i].mod.name, g.mods[i].mod.enabled ? "on" : "off").c_str());
}

// Runs a command line hidden and waits; returns its exit code (-1 when it could not start).
int RunHidden(const std::wstring& exe, std::wstring cmd) {
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS,
                        nullptr, nullptr, &si, &pi))
        return -1;
    CloseHandle(pi.hThread);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    return int(code);
}

fs::path SevenZip() {
    for (const wchar_t* v : {L"ProgramFiles", L"ProgramW6432", L"ProgramFiles(x86)"})
        if (const wchar_t* pf = _wgetenv(v))
            if (fs::path p = fs::path(pf) / "7-Zip" / "7z.exe"; fs::exists(p))
                return p;
    return {};
}

bool IsArchive(const fs::path& p) {
    const std::string e = Lower(p.extension().string());
    return e == ".zip" || e == ".7z" || e == ".rar";
}

// Installs a mod from an archive or a folder into mods\. Returns what to tell the player.
std::string InstallMod(const fs::path& src, bool& ok) {
    ok = false;
    std::error_code ec;
    const fs::path tmp = g.dir / ".mod_install"; // (same drive as mods\: the move is a rename)
    fs::remove_all(tmp, ec);
    std::string stem = src.filename().string();
    if (!fs::is_directory(src, ec))
        stem = src.stem().string();
    const fs::path into = tmp / stem;
    fs::create_directories(into, ec);
    if (fs::is_directory(src, ec)) {
        fs::copy(src, into, fs::copy_options::recursive, ec);
        if (ec)
            return "Could not copy that folder: " + ec.message();
    } else if (IsArchive(src)) {
        wchar_t sys[MAX_PATH]{};
        GetSystemDirectoryW(sys, MAX_PATH);
        const fs::path tar = fs::path(sys) / "tar.exe";
        int rc = fs::exists(tar) ? RunHidden(tar.wstring(), L"\"" + tar.wstring() + L"\" -xf \"" + src.wstring() + L"\" -C \"" +
                                                                into.wstring() + L"\"")
                                 : -1;
        if (rc != 0) {
            fs::remove_all(into, ec);
            fs::create_directories(into, ec);
            const fs::path sz = SevenZip();
            rc = sz.empty() ? -1 : RunHidden(sz.wstring(), L"\"" + sz.wstring() + L"\" x -y \"-o" + into.wstring() + L"\" \"" + src.wstring() + L"\"");
            if (rc != 0) {
                fs::remove_all(tmp, ec);
                return sz.empty() ? "Windows could not open this archive. Install 7-Zip (7-zip.org) and try again, or "
                                    "extract it yourself and drop the folder."
                                  : "Could not open this archive (it may be damaged or password-protected).";
            }
        }
    } else {
        fs::remove_all(tmp, ec);
        return "Not a mod: drop a .zip, .7z or .rar archive, or a mod's folder.";
    }
    // An archive that holds one folder (Mod Name\...): that folder is the mod, not a wrapper around it. (Layout folders
    // such as romfs, 010060700AC50000 or the game's own DATA, UI, ... are kept.)
    for (int depth = 0; depth < 2; depth++) {
        std::vector<fs::path> entries;
        for (auto& e : fs::directory_iterator(into, ec))
            entries.push_back(e.path());
        if (entries.size() != 1 || !fs::is_directory(entries[0], ec))
            break;
        const std::string n = entries[0].filename().string(), l = Lower(n);
        const bool layout = l == "romfs" || l == "exefs" || l == "atmosphere" || l == "contents" || l == Lower(hle::mods::kTitleId) ||
                            std::none_of(n.begin(), n.end(), [](char c) { return std::islower(u8(c)); });
        if (layout)
            break;
        const fs::path up = tmp / ("~" + stem);
        fs::rename(entries[0], up, ec);
        if (ec)
            break;
        fs::remove_all(into, ec);
        fs::rename(up, into, ec);
    }
    // which mods it holds: the game's own mod finder, on the extracted copy
    std::vector<hle::mods::Mod> found = hle::mods::Scan(false, tmp);
    auto useful = [](const std::vector<hle::mods::Mod>& v) {
        return std::any_of(v.begin(), v.end(), [](const hle::mods::Mod& m) { return !m.romfs.empty() || !m.unsupported.empty(); });
    };
    if (!useful(found)) // an emulator's whole mods folder (010060700AC50000\<mod>\romfs): its folders are the mods
        found = hle::mods::Scan(false, into);
    if (!useful(found)) {
        fs::remove_all(tmp, ec);
        return "No game files in " + src.filename().string() + ": this does not look like an MUA3 mod for the Switch version.";
    }
    std::vector<std::string> names, replaced;
    bool code_only = false;
    for (auto& m : found) {
        if (m.romfs.empty() && m.unsupported.empty())
            continue;
        if (m.romfs.empty())
            code_only = true;
        const fs::path dest = ModsDir() / m.name;
        hle::mods::EnsureFolder(ModsDir());
        if (fs::exists(dest, ec)) { // an update of a mod already installed: the old copy goes to the Recycle Bin
            if (!ui::Recycle(dest))
                fs::remove_all(dest, ec);
            replaced.push_back(m.name);
        }
        fs::rename(m.folder, dest, ec);
        if (ec) {
            ec.clear();
            fs::copy(m.folder, dest, fs::copy_options::recursive, ec);
        }
        if (ec) {
            fs::remove_all(tmp, ec);
            return "Could not move the mod into the mods folder: " + ec.message();
        }
        names.push_back(m.name);
    }
    fs::remove_all(tmp, ec);
    hle::mods::Scan(true, ModsDir()); // new mods go to the bottom of order.txt, on
    ok = true;
    std::string list;
    for (auto& n : names)
        list += (list.empty() ? "" : ", ") + n;
    {
        std::lock_guard lk(g.mutex);
        g.installed_mod = names.empty() ? std::string() : names.back();
    }
    std::string msg = (replaced.empty() ? "Installed " : "Updated ") + list + ".";
    if (code_only)
        msg += " Its code patches cannot work in this port.";
    return msg + " Applies the next time the game starts.";
}

void StartInstall(const fs::path& src) {
    if (g.installing)
        return;
    if (g.install.joinable())
        g.install.join();
    g.installing = true;
    g.install_name = src.filename().string();
    g.install = std::thread([src] {
        bool ok = false;
        const std::string msg = InstallMod(src, ok);
        LOG_INFO("Launcher", "mod install {}: {}", src.filename().string(), msg);
        std::lock_guard lk(g.mutex);
        g.install_result = msg;
        g.mods_note_col = ok ? col::green : col::red;
        g.install_done = true;
        g.installing = false;
    });
}

// ---- drawing: the frame
struct Layout {
    float W, H;
    float panel_top, panel_bottom; // the left panel's right edge at the top and the bottom
    float x0, x1, y0, y1;          // the section area
};

void Background(ImDrawList* dl, const Layout& L) {
    const float W = L.W, H = L.H;
    dl->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(W, H), IM_COL32(9, 24, 58, 255), IM_COL32(12, 34, 80, 255),
                                IM_COL32(2, 5, 12, 255), IM_COL32(2, 4, 10, 255));
    Glow(dl, ImVec2(W * 0.74f, H * 0.16f), U(520), IM_COL32(40, 100, 210, 70)); // a soft glow, upper right
    for (auto& s : g_stars) {
        const float tw = 0.75f + 0.25f * std::sin(g.t * 1.3f + s.phase);
        dl->AddCircleFilled(ImVec2(s.x * W, s.y * H), U(s.r), Alpha(IM_COL32(215, 228, 255, 255), int((60 + 190 * s.a) * tw)), 6);
    }
    // the left panel: a blue nebula, its right edge leaning
    Quad(dl, ImVec2(0, 0), ImVec2(L.panel_top, 0), ImVec2(L.panel_bottom, H), ImVec2(0, H), IM_COL32(18, 104, 228, 255),
         IM_COL32(13, 86, 205, 255), IM_COL32(4, 14, 34, 255), IM_COL32(3, 10, 24, 255));
    Glow(dl, ImVec2(U(150), U(90)), U(300), IM_COL32(120, 190, 255, 80));
    for (size_t i = 0; i < g_stars.size(); i += 3) { // a few stars over the panel
        const Star& s = g_stars[i];
        const float x = s.x * L.panel_bottom * 0.95f, y = s.y * H;
        dl->AddCircleFilled(ImVec2(x, y), U(s.r * 0.8f), Alpha(IM_COL32(220, 235, 255, 255), int(40 + 120 * s.a)), 6);
    }
    // the yellow stripe and its dark edge
    const float sw = U(36), dw = U(10);
    Quad(dl, ImVec2(L.panel_top, 0), ImVec2(L.panel_top + sw, 0), ImVec2(L.panel_bottom + sw, H), ImVec2(L.panel_bottom, H),
         col::yellow, col::yellow, col::yellow2, col::yellow2);
    Quad(dl, ImVec2(L.panel_top + sw, 0), ImVec2(L.panel_top + sw + dw, 0), ImVec2(L.panel_bottom + sw + dw, H),
         ImVec2(L.panel_bottom + sw, H), col::ink, col::ink, col::ink, col::ink);
}

void Title(ImDrawList* dl) {
    {
        Font f(g_body, 13);
        Spaced(dl, ImVec2(U(46), U(46)), IM_COL32(207, 226, 255, 255), "NATIVE PC PORT", U(4));
    }
    Font f(g_head, 44);
    Shadowed(dl, ImVec2(U(44), U(62)), col::text, "ULTIMATE");
    Shadowed(dl, ImVec2(U(44), U(104)), col::text, "ALLIANCE");
    Shadowed(dl, ImVec2(U(44) + ImGui::CalcTextSize("ALLIANCE ").x, U(104)), col::yellow, "3");
}

std::string MenuHint(int i) {
    switch (i) {
    case kMods: {
        int on = 0;
        for (auto& r : g.mods)
            on += r.mod.enabled;
        return !g.mods_on ? "OFF  ·  DROP A .ZIP TO ADD" : fmt::format("{} ON  ·  DROP A .ZIP TO ADD", on);
    }
    case kSettings: return "GRAPHICS  ·  AUDIO  ·  CONTROLS";
    default: return "";
    }
}

// The main menu; returns the entry activated this frame (or -1).
int Menu(ImDrawList* dl, const Layout& L) {
    int activated = -1;
    const float y0 = U(240), step = U(66);
    for (int i = 0; i < kSections; i++) {
        const float y = y0 + step * i;
        const ImVec2 a(0, y + U(8)), b(L.panel_top - U(90), y + U(58));
        ImGui::SetCursorScreenPos(a);
        ImGui::PushID(i);
        const bool click = ImGui::InvisibleButton("item", ImVec2(b.x - a.x, b.y - a.y));
        if (ImGui::IsItemHovered() && ImGui::GetIO().MouseDelta.x * ImGui::GetIO().MouseDelta.x + ImGui::GetIO().MouseDelta.y * ImGui::GetIO().MouseDelta.y > 0)
            g.sel = i, g.in_section = false;
        if (click)
            g.sel = i, activated = i;
        ImGui::PopID();
        const bool on = g.sel == i;
        if (on) {
            Bar(dl, a, b, U(26), col::yellow, col::yellow2);
            const float cx = U(40), cy = (a.y + b.y) * 0.5f;
            if (!g.in_section)
                dl->AddTriangleFilled(ImVec2(cx, cy - U(9)), ImVec2(cx + U(16), cy), ImVec2(cx, cy + U(9)), col::ink);
        }
        {
            Font f(g_head, 34);
            const float ty = (a.y + b.y) * 0.5f - ImGui::GetFontSize() * 0.55f;
            if (on)
                dl->AddText(ImVec2(U(84), ty), col::ink, kSectionNames[i]);
            else
                Shadowed(dl, ImVec2(U(84), ty), col::text, kSectionNames[i]);
        }
        if (!on) {
            const std::string hint = MenuHint(i);
            if (!hint.empty()) {
                Font f(g_body, 12);
                Spaced(dl, ImVec2(U(86), y + U(52)), col::dim, hint.c_str(), U(1));
            }
        }
    }
    if (!g.in_section) {
        if (NavUp())
            g.sel = (g.sel + kSections - 1) % kSections;
        if (NavDown())
            g.sel = (g.sel + 1) % kSections;
        if (NavAccept())
            activated = g.sel;
    }
    return activated;
}

// A card: a dark panel with cut corners.
void Card(ImDrawList* dl, ImVec2 a, ImVec2 b) {
    const float c = U(22);
    const ImVec2 pts[6] = {ImVec2(a.x + c, a.y), ImVec2(b.x, a.y), ImVec2(b.x, b.y - c), ImVec2(b.x - c, b.y), ImVec2(a.x, b.y), ImVec2(a.x, a.y + c)};
    dl->AddConvexPolyFilled(pts, 6, col::card);
    dl->AddPolyline(pts, 6, IM_COL32(255, 255, 255, 30), ImDrawFlags_Closed, U(1));
}

// A big yellow slanted button; true when clicked (or `focused` and accepted).
bool BigButton(ImDrawList* dl, const char* id, const char* label, ImVec2 a, ImVec2 b, bool focused) {
    ImGui::SetCursorScreenPos(a);
    const bool click = ImGui::InvisibleButton(id, ImVec2(b.x - a.x, b.y - a.y));
    const bool hov = ImGui::IsItemHovered();
    const float glow = hov || focused ? 1.0f : 0.0f;
    if (glow > 0)
        Slant(dl, ImVec2(a.x - U(4), a.y - U(4)), ImVec2(b.x + U(4), b.y + U(4)), U(18), IM_COL32(255, 214, 10, 60), IM_COL32(255, 214, 10, 60));
    Slant(dl, a, b, U(16), col::yellow, col::yellow2);
    Font f(g_head, 28);
    const ImVec2 ts = ImGui::CalcTextSize(label);
    dl->AddText(ImVec2((a.x + b.x - ts.x) * 0.5f, (a.y + b.y - ts.y) * 0.5f), col::ink, label);
    return click;
}

void InfoRow(ImDrawList* dl, float x0, float x1, float& y, const char* label, const std::string& value, ImU32 vc) {
    {
        Font f(g_head, 13);
        Spaced(dl, ImVec2(x0, y + U(12)), IM_COL32(214, 226, 255, 255), label, U(1.5f));
    }
    Font f(g_body, 16);
    const ImVec2 ts = ImGui::CalcTextSize(value.c_str());
    dl->AddText(ImVec2(x1 - ts.x, y + U(10)), vc, value.c_str());
    y += U(42);
    dl->AddLine(ImVec2(x0, y), ImVec2(x1, y), col::line, U(1));
}

// ---- PLAY
void PlaySection(ImDrawList* dl, const Layout& L, bool activate) {
    const bool exe = fs::exists(g.dir / "mua3.exe");
    const float banner = UpdateBannerHeight();
    const ImVec2 a(L.x1 - U(520), L.y0 + U(56)), b(L.x1, L.y0 + U(56) + U(g.ended.empty() ? 384 : 458) + banner);
    Card(dl, a, b);
    const float x0 = a.x + U(34), x1 = b.x - U(34);
    float y = a.y + U(28);
    {
        Font f(g_head, 30);
        dl->AddText(ImVec2(x0, y), col::yellow, g.game ? "GAME RUNNING" : exe ? "READY TO PLAY" : "NOT BUILT YET");
    }
    y += U(40);
    {
        Font f(g_body, 14);
        dl->AddText(ImVec2(x0, y), col::dim,
                    g.game ? "The launcher waits here until the game closes."
                    : exe  ? "The game is built and installed."
                    : g.host->installed.dev ? "mua3.exe is missing from this developer folder."
                           : "mua3.exe is missing: SETTINGS > SYSTEM > UPDATE OR REPAIR builds it again.");
    }
    y += U(30);
    std::string version, dlc;
    {
        std::lock_guard lk(g.mutex);
        version = g.probed ? g.game_version : "...";
        dlc = g.probed ? g.dlc : "...";
    }
    int on = 0;
    for (auto& r : g.mods)
        on += r.mod.enabled;
    InfoRow(dl, x0, x1, y, "GAME VERSION", version, version.find("supported") != std::string::npos ? col::green : col::text);
    InfoRow(dl, x0, x1, y, "DLC", dlc, col::text);
    InfoRow(dl, x0, x1, y, "MODS", !g.mods_on ? "Off" : g.mods.empty() ? "None" : fmt::format("{} of {} on", on, g.mods.size()),
            g.mods_on && on ? col::yellow : col::text);
    const std::string display = fmt::format("{} x {}  ·  {}  ·  {} fps", g.cfg.width, g.cfg.height,
                                            g.cfg.fullscreen ? "Fullscreen" : "Windowed", g.cfg.fps_target);
    InfoRow(dl, x0, x1, y, "DISPLAY", display, col::text);
    y += U(24);
    if (!g.ended.empty()) {
        const ImVec2 ea(x0, y), eb(x1, y + U(58));
        dl->AddRectFilled(ea, eb, IM_COL32(120, 30, 24, 120));
        dl->AddRect(ea, eb, Alpha(col::red, 160));
        Font f(g_body, 14);
        ImGui::PushClipRect(ea, eb, true);
        dl->AddText(nullptr, 0, ImVec2(ea.x + U(12), ea.y + U(8)), col::text, g.ended.c_str(), nullptr, eb.x - ea.x - U(150));
        ImGui::PopClipRect();
        ImGui::SetCursorScreenPos(ImVec2(eb.x - U(132), ea.y + U(14)));
        if (g.crashed && ImGui::InvisibleButton("report", ImVec2(U(120), U(30))))
            g.ended = "!report";
        if (g.crashed) {
            Font h(g_head, 14);
            dl->AddText(ImVec2(eb.x - U(130), ea.y + U(20)), ImGui::IsItemHovered() ? col::yellow : col::yellow2, "REPORT IT >");
        }
        y += U(74);
    }
    if (banner > 0) {
        using St = updater::Updater::State;
        const St st = g.update.state;
        const ImVec2 ua(x0, y), ub(x1, y + U(58));
        dl->AddRectFilled(ua, ub, IM_COL32(20, 60, 40, 150));
        dl->AddRect(ua, ub, Alpha(st == St::Failed ? col::red : col::green, 170));
        std::string head, sub, err;
        std::string ver;
        {
            std::lock_guard lk(g.update.mutex);
            ver = g.update.release.version;
            err = g.update.error;
        }
        const bool elsewhere = ManagedElsewhere();
        switch (st) {
        case St::Available:
            head = "NEW VERSION " + ver;
            sub = elsewhere ? "Update it in RecompLauncher." : "Downloads it and builds your game again (your saves stay).";
            break;
        case St::Downloading: head = fmt::format("DOWNLOADING {}  {:.0f}%", ver, g.update.progress * 100); break;
        case St::Unpacking: head = "UNPACKING " + ver; break;
        case St::Ready: head = "STARTING THE UPDATE"; break;
        default: head = "UPDATE FAILED", sub = err; break;
        }
        {
            Font f(g_head, 17);
            dl->AddText(ImVec2(ua.x + U(12), ua.y + U(8)), st == St::Failed ? col::red : col::green, head.c_str());
        }
        if (!sub.empty()) {
            Font f(g_body, 12.5f);
            ImGui::PushClipRect(ua, ImVec2(ub.x - U(140), ub.y), true);
            dl->AddText(ImVec2(ua.x + U(12), ua.y + U(32)), col::dim, sub.c_str());
            ImGui::PopClipRect();
        }
        if (st == St::Downloading) {
            const float w = (ub.x - ua.x - U(24)) * g.update.progress;
            dl->AddRectFilled(ImVec2(ua.x + U(12), ub.y - U(14)), ImVec2(ua.x + U(12) + w, ub.y - U(9)), col::green);
        }
        if ((st == St::Available && !elsewhere) || st == St::Failed) {
            ImGui::SetCursorScreenPos(ImVec2(ub.x - U(132), ua.y + U(14)));
            const bool click = ImGui::InvisibleButton("update", ImVec2(U(120), U(30)));
            Font h(g_head, 14);
            dl->AddText(ImVec2(ub.x - U(128), ua.y + U(20)), ImGui::IsItemHovered() ? col::yellow : col::yellow2,
                        st == St::Failed ? "TRY AGAIN >" : "UPDATE NOW >");
            if (click || Pressed({ImGuiKey_GamepadFaceUp, ImGuiKey_U}, false)) {
                if (st == St::Failed) {
                    g.update.state = St::Idle;
                    g.update.Check(g.host->version);
                } else {
                    g.update_started = true;
                    g.update.Start(g.host->installed.source_dir);
                }
            }
        }
        y += U(74);
    }
    if (exe && !g.game && !g.diag.Running() && (BigButton(dl, "play", "PLAY", ImVec2(x0, y), ImVec2(x1, y + U(58)), g.in_section || activate) ||
                           activate))
        Launch();
}

// ---- MODS
bool Toggle(ImDrawList* dl, const char* id, ImVec2 p, bool on) {
    const ImVec2 sz(U(42), U(22));
    ImGui::SetCursorScreenPos(p);
    const bool click = ImGui::InvisibleButton(id, sz);
    dl->AddRectFilled(p, ImVec2(p.x + sz.x, p.y + sz.y), on ? col::yellow : IM_COL32(58, 63, 74, 255), sz.y * 0.5f);
    const float x = on ? p.x + sz.x - sz.y * 0.5f : p.x + sz.y * 0.5f;
    dl->AddCircleFilled(ImVec2(x, p.y + sz.y * 0.5f), sz.y * 0.5f - U(3), on ? col::ink : IM_COL32_WHITE, 20);
    return click;
}

bool TextLink(ImDrawList* dl, const char* id, ImVec2 p, const char* label, float size) {
    Font f(g_head, size);
    const ImVec2 ts = ImGui::CalcTextSize(label);
    ImGui::SetCursorScreenPos(p);
    const bool click = ImGui::InvisibleButton(id, ts);
    dl->AddText(p, ImGui::IsItemHovered() ? IM_COL32_WHITE : col::yellow, label);
    return click;
}

void Dashed(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 c) {
    const float d = U(8), gap = U(6), t = U(2);
    for (float x = a.x; x < b.x; x += d + gap) {
        dl->AddLine(ImVec2(x, a.y), ImVec2(std::min(x + d, b.x), a.y), c, t);
        dl->AddLine(ImVec2(x, b.y), ImVec2(std::min(x + d, b.x), b.y), c, t);
    }
    for (float y = a.y; y < b.y; y += d + gap) {
        dl->AddLine(ImVec2(a.x, y), ImVec2(a.x, std::min(y + d, b.y)), c, t);
        dl->AddLine(ImVec2(b.x, y), ImVec2(b.x, std::min(y + d, b.y)), c, t);
    }
}

void ArrowGlyph(ImDrawList* dl, ImVec2 c, bool up, ImU32 col) {
    const float s = U(6), d = up ? -1.0f : 1.0f;
    dl->AddTriangleFilled(ImVec2(c.x - s, c.y - d * s * 0.5f), ImVec2(c.x + s, c.y - d * s * 0.5f), ImVec2(c.x, c.y + d * s * 0.6f), col);
}

void AddModDialog() {
    const auto files = ui::FileDialog(false, true, L"Add a mod", {{L"Mod archives (.zip, .7z, .rar)", L"*.zip;*.7z;*.rar"}, {L"All files", L"*.*"}}, {});
    if (!files.empty())
        StartInstall(files[0]); // (one at a time; the others: drop them)
}

void ModsSection(ImDrawList* dl, const Layout& L) {
    // an install finished: its result, and the list again
    bool finished = false;
    {
        std::lock_guard lk(g.mutex);
        if (g.install_done) {
            g.install_done = false;
            g.mods_note = g.install_result;
            finished = true;
        }
    }
    if (finished) {
        if (g.install.joinable())
            g.install.join();
        ScanMods();
        SelectInstalled();
    }
    const float x0 = L.x0, x1 = L.x1;
    float y = L.y0;
    {
        Font f(g_head, 40);
        Shadowed(dl, ImVec2(x0, y), col::yellow, "MODS");
    }
    {
        Font f(g_head, 14);
        const char* label = "USE MODS";
        const float tw = SpacedWidth(label, U(1.5f));
        Spaced(dl, ImVec2(x1 - U(42) - U(14) - tw, y + U(18)), col::text, label, U(1.5f));
        if (Toggle(dl, "master", ImVec2(x1 - U(42), y + U(16)), g.mods_on)) {
            g.mods_on = !g.mods_on;
            SaveMods(g.mods_on ? "Mods are on." : "Mods are off: the game starts without them.");
        }
    }
    y += U(62);
    // the drop zone
    const ImVec2 za(x0, y), zb(x1, y + U(52));
    dl->AddRectFilled(za, zb, IM_COL32(255, 214, 10, 12));
    Dashed(dl, za, zb, IM_COL32(255, 214, 10, 140));
    if (g.installing) {
        Font f(g_body, 14);
        const float t = g.t * 6;
        const ImVec2 c(za.x + U(26), (za.y + zb.y) * 0.5f);
        dl->PathArcTo(c, U(8), t, t + 4.4f, 20);
        dl->PathStroke(col::yellow, 0, U(2.5f));
        dl->AddText(ImVec2(c.x + U(20), c.y - ImGui::GetFontSize() * 0.5f), col::text, ("Installing " + g.install_name + "...").c_str());
    } else {
        Font f(g_body, 14);
        const char* t1 = "Drop a mod (.zip / .7z / .rar or a folder) here   ·   ";
        const char* l1 = "ADD MOD...";
        const char* l2 = "OPEN MODS FOLDER";
        float w = ImGui::CalcTextSize(t1).x + U(14) + ImGui::CalcTextSize("   ·   ").x;
        {
            Font h(g_head, 14);
            w += ImGui::CalcTextSize(l1).x + ImGui::CalcTextSize(l2).x;
        }
        float x = (za.x + zb.x - w) * 0.5f;
        const float ty = (za.y + zb.y) * 0.5f - ImGui::GetFontSize() * 0.5f;
        dl->AddText(ImVec2(x, ty), IM_COL32(207, 224, 255, 255), t1);
        x += ImGui::CalcTextSize(t1).x;
        if (TextLink(dl, "add", ImVec2(x, ty), l1, 14))
            AddModDialog();
        {
            Font h(g_head, 14);
            x += ImGui::CalcTextSize(l1).x;
        }
        dl->AddText(ImVec2(x, ty), IM_COL32(207, 224, 255, 255), "   ·   ");
        x += ImGui::CalcTextSize("   ·   ").x;
        if (TextLink(dl, "open", ImVec2(x, ty), l2, 14)) {
            hle::mods::EnsureFolder(ModsDir());
            ShellExecuteW(nullptr, L"open", ModsDir().wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
    }
    y += U(66);
    {
        Font f(g_head, 12);
        int on = 0;
        for (auto& r : g.mods)
            on += r.mod.enabled;
        const std::string label = g.mods.empty() ? std::string("LOAD ORDER  ·  LOWER WINS")
                                                 : fmt::format("LOAD ORDER  ·  LOWER WINS  ·  {} MODS, {} ON", g.mods.size(), on);
        Spaced(dl, ImVec2(x0, y), col::dim, label.c_str(), U(1.6f));
    }
    y += U(24);
    // the list (scrolls)
    const float list_bottom = L.y1 - U(34);
    ImGui::SetCursorScreenPos(ImVec2(x0 - U(4), y));
    ImGui::BeginChild("modlist", ImVec2(x1 - x0 + U(8), list_bottom - y), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
    ImDrawList* ldl = ImGui::GetWindowDrawList();
    const float row_h = U(64), gap = U(8);
    int move = 0, move_i = -1, toggle = -1;
    if (g.mods.empty()) {
        Font f(g_body, 15);
        ImGui::SetCursorPos(ImVec2(U(8), U(10)));
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(col::dim));
        ImGui::TextWrapped("No mods yet. Drop a mod's archive or folder on this window, or press ADD MOD. Mods made for the "
                           "Switch version (retextures, data and UI files) work as they come.");
        ImGui::PopStyleColor();
    }
    for (int i = 0; i < int(g.mods.size()); i++) {
        ModRow& r = g.mods[i];
        ImGui::PushID(i);
        ImGui::SetCursorPos(ImVec2(U(4), U(2) + i * (row_h + gap)));
        const ImVec2 a = ImGui::GetCursorScreenPos(), b(a.x + x1 - x0, a.y + row_h);
        const bool sel = g.in_section && g.mod_sel == i;
        Slant(ldl, a, b, U(10), sel ? IM_COL32(40, 54, 96, 235) : IM_COL32(20, 34, 70, 200),
              sel ? IM_COL32(40, 54, 96, 235) : IM_COL32(20, 34, 70, 200));
        if (sel) {
            const ImVec2 pts[4] = {ImVec2(a.x + U(10), a.y), b + ImVec2(0, -row_h), ImVec2(b.x - U(10), b.y), ImVec2(a.x, b.y)};
            ldl->AddPolyline(pts, 4, col::yellow, ImDrawFlags_Closed, U(2));
        }
        const float cy = (a.y + b.y) * 0.5f;
        {
            Font f(g_body, 15);
            const std::string n = std::to_string(i + 1);
            ldl->AddText(ImVec2(a.x + U(18), cy - ImGui::GetFontSize() * 0.5f), col::faint, n.c_str());
        }
        if (Toggle(ldl, "on", ImVec2(a.x + U(46), cy - U(11)), r.mod.enabled))
            toggle = i, g.mod_sel = i;
        float tx = a.x + U(106);
        // preview
        if (!r.tex_tried && !r.preview.empty()) {
            r.tex_tried = true;
            r.tex = ui::LoadImageTexture(g.host->dev, r.preview);
        }
        const float right_w = U(96);
        if (r.tex.srv) {
            const float th = row_h - U(14), tw = std::min(th * r.tex.w / std::max(1, r.tex.h), U(90));
            const ImVec2 pa(b.x - right_w - tw - U(12), a.y + U(7));
            ldl->AddImage(ImTextureRef(reinterpret_cast<ImTextureID>(r.tex.srv)), pa, ImVec2(pa.x + tw, pa.y + th));
            ImGui::SetCursorScreenPos(pa);
            ImGui::InvisibleButton("pv", ImVec2(tw, th));
            if (ImGui::IsItemHovered()) {
                ImGui::BeginTooltip();
                const float big = std::min(U(360), float(r.tex.w));
                ImGui::Image(ImTextureRef(reinterpret_cast<ImTextureID>(r.tex.srv)), ImVec2(big, big * r.tex.h / std::max(1, r.tex.w)));
                ImGui::EndTooltip();
            }
        }
        {
            Font f(g_head, 18);
            std::string name = r.mod.name;
            for (char& c : name)
                c = char(std::toupper(u8(c)));
            ldl->AddText(ImVec2(tx, a.y + U(10)), r.mod.enabled && g.mods_on ? col::text : col::faint, name.c_str());
        }
        {
            Font f(g_body, 12.5f);
            std::string sub;
            ImU32 sc = col::dim;
            if (r.mod.romfs.empty()) {
                sub = "No game files found in this folder: nothing to load.";
                sc = col::amber;
            } else if (r.counted) {
                sub = fmt::format("{} file{}", r.files, r.files == 1 ? "" : "s");
                if (r.replaced)
                    sub += fmt::format(" · {} replaced", r.replaced);
                if (r.added)
                    sub += fmt::format(" · {} added", r.added);
                if (!r.mod.enabled)
                    sub += " · off";
            } else {
                sub = "Counting files...";
            }
            float sx = tx;
            const float sy = a.y + U(36);
            ldl->AddText(ImVec2(sx, sy), sc, sub.c_str());
            sx += ImGui::CalcTextSize(sub.c_str()).x;
            std::string warn;
            if (r.counted && r.lost > 0 && r.mod.enabled)
                warn = r.lost == r.files ? fmt::format(" · overridden by {}", r.lost_to)
                                         : fmt::format(" · {} of them overridden by {}", r.lost, r.lost_to);
            if (!r.mod.unsupported.empty())
                warn += std::string(" · ") + (r.mod.unsupported.size() == 1 ? "1 part" : fmt::format("{} parts", r.mod.unsupported.size())) +
                        " not supported here (code patches / cheats)";
            if (!warn.empty()) {
                ImGui::PushClipRect(ImVec2(sx, a.y), ImVec2(b.x - right_w - U(110), b.y), true);
                ldl->AddText(ImVec2(sx, sy), r.mod.unsupported.empty() ? col::yellow : col::amber, warn.c_str());
                ImGui::PopClipRect();
            }
            if (!r.mod.unsupported.empty() || r.lost > 0) { // the full text on hover
                ImGui::SetCursorScreenPos(ImVec2(tx, sy));
                ImGui::InvisibleButton("why", ImVec2(std::max(U(10), b.x - right_w - U(110) - tx), U(18)));
                if (ImGui::IsItemHovered()) {
                    ImGui::BeginTooltip();
                    ImGui::PushTextWrapPos(U(460));
                    if (r.lost > 0)
                        ImGui::Text("%s overridden by %s: the lower mod in the list wins.",
                                    r.lost == r.files ? "All its files are" : fmt::format("{} of its files are", r.lost).c_str(),
                                    r.lost_to.c_str());
                    for (auto& u : r.mod.unsupported)
                        ImGui::Text("Not supported: %s", u.c_str());
                    ImGui::PopTextWrapPos();
                    ImGui::EndTooltip();
                }
            }
        }
        // up, down, remove
        const float bx = b.x - right_w;
        auto small = [&](const char* id, float x, int kind, bool enabled) {
            ImGui::SetCursorScreenPos(ImVec2(x, cy - U(14)));
            ImGui::BeginDisabled(!enabled);
            const bool click = ImGui::InvisibleButton(id, ImVec2(U(26), U(28)));
            ImGui::EndDisabled();
            const ImU32 c = !enabled ? IM_COL32(90, 100, 125, 255) : ImGui::IsItemHovered() ? col::yellow : col::dim;
            const ImVec2 cc(x + U(13), cy);
            if (kind == 0 || kind == 1)
                ArrowGlyph(ldl, cc, kind == 0, c);
            else {
                const float s = U(5.5f);
                ldl->AddLine(ImVec2(cc.x - s, cc.y - s), ImVec2(cc.x + s, cc.y + s), c, U(2));
                ldl->AddLine(ImVec2(cc.x - s, cc.y + s), ImVec2(cc.x + s, cc.y - s), c, U(2));
            }
            return click && enabled;
        };
        if (small("up", bx, 0, i > 0))
            move = -1, move_i = i;
        ImGui::SetItemTooltip("Load earlier");
        if (small("down", bx + U(28), 1, i + 1 < int(g.mods.size())))
            move = 1, move_i = i;
        ImGui::SetItemTooltip("Load later (wins over the mods above it)");
        if (small("rm", bx + U(60), 2, true))
            g.confirm_remove = i;
        ImGui::SetItemTooltip("Remove (to the Recycle Bin)");
        ImGui::PopID();
    }
    if (g.in_section && !g.mods.empty()) { // keep the keyboard row in view
        const float top = U(2) + g.mod_sel * (row_h + gap);
        if (top < ImGui::GetScrollY())
            ImGui::SetScrollY(top);
        else if (top + row_h > ImGui::GetScrollY() + ImGui::GetWindowHeight())
            ImGui::SetScrollY(top + row_h - ImGui::GetWindowHeight() + U(4));
    }
    ImGui::Dummy(ImVec2(1, U(4)));
    ImGui::EndChild();
    // keyboard / controller in the list
    if (g.in_section && g.confirm_remove < 0 && !g.mods.empty()) {
        const bool moving = ImGui::IsKeyDown(ImGuiKey_LeftShift) || ImGui::IsKeyDown(ImGuiKey_RightShift) ||
                            ImGui::IsKeyDown(ImGuiKey_GamepadFaceUp);
        if (NavUp())
            moving ? (void)(move = -1, move_i = g.mod_sel) : (void)(g.mod_sel = std::max(0, g.mod_sel - 1));
        if (NavDown())
            moving ? (void)(move = 1, move_i = g.mod_sel) : (void)(g.mod_sel = std::min(int(g.mods.size()) - 1, g.mod_sel + 1));
        if (Pressed({ImGuiKey_Space, ImGuiKey_GamepadFaceLeft, ImGuiKey_Enter, ImGuiKey_GamepadFaceDown}, false))
            toggle = g.mod_sel;
        if (Pressed({ImGuiKey_Delete}, false))
            g.confirm_remove = g.mod_sel;
    }
    if (toggle >= 0)
        ToggleMod(toggle);
    if (move_i >= 0)
        MoveMod(move_i, move);
    // the line under the list
    {
        Font f(g_body, 13.5f);
        const std::string note = g.mods_note.empty() ? std::string("Changes apply the next time the game starts. Code patches "
                                                                    "(exefs) and cheats cannot work in this port.")
                                                     : g.mods_note;
        ImGui::PushClipRect(ImVec2(x0, list_bottom), ImVec2(x1, L.y1), true);
        dl->AddText(ImVec2(x0, list_bottom + U(10)), g.mods_note.empty() ? col::dim : g.mods_note_col, note.c_str());
        ImGui::PopClipRect();
    }
    // remove: asks first
    if (g.confirm_remove >= 0 && g.confirm_remove < int(g.mods.size())) {
        ImGui::OpenPopup("remove");
        ImGui::SetNextWindowPos(ImVec2(L.W * 0.5f, L.H * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(U(480), 0));
        ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0.05f, 0.08f, 0.17f, 0.98f));
        ImGui::PushStyleColor(ImGuiCol_Border, ImGui::ColorConvertU32ToFloat4(col::yellow));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(U(24), U(20)));
        if (ImGui::BeginPopupModal("remove", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize)) {
            const std::string name = g.mods[g.confirm_remove].mod.name;
            {
                Font f(g_head, 22);
                ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(col::yellow), "REMOVE %s?", name.c_str());
            }
            {
                Font f(g_body, 15);
                ImGui::PushTextWrapPos(U(430));
                ImGui::TextUnformatted("Its folder goes to the Recycle Bin (you can restore it from there).");
                ImGui::PopTextWrapPos();
            }
            ImGui::Dummy(ImVec2(1, U(10)));
            ImDrawList* pdl = ImGui::GetWindowDrawList();
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const bool yes = BigButton(pdl, "yes", "REMOVE", p, ImVec2(p.x + U(200), p.y + U(46)), false) ||
                             Pressed({ImGuiKey_Enter, ImGuiKey_GamepadFaceDown}, false);
            ImGui::SetCursorScreenPos(ImVec2(p.x + U(220), p.y));
            const bool no = ImGui::InvisibleButton("no", ImVec2(U(200), U(46))) || NavBack();
            {
                Slant(pdl, ImVec2(p.x + U(220), p.y), ImVec2(p.x + U(420), p.y + U(46)), U(16), IM_COL32(58, 63, 74, 255), IM_COL32(58, 63, 74, 255));
                Font f(g_head, 22);
                const ImVec2 ts = ImGui::CalcTextSize("CANCEL");
                pdl->AddText(ImVec2(p.x + U(320) - ts.x * 0.5f, p.y + U(23) - ts.y * 0.5f), col::text, "CANCEL");
            }
            ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + U(52)));
            ImGui::Dummy(ImVec2(1, 1));
            if (yes) {
                const fs::path folder = g.mods[g.confirm_remove].mod.folder;
                const bool ok = ui::Recycle(folder);
                LOG_INFO("Launcher", "mod removed: {} ({})", name, ok ? "Recycle Bin" : "FAILED");
                g.confirm_remove = -1;
                ImGui::CloseCurrentPopup();
                ScanMods();
                SaveMods(ok ? fmt::format("Removed {} (it is in the Recycle Bin).", name).c_str() : "Could not remove it (is it open?).");
            } else if (no) {
                g.confirm_remove = -1;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(2);
    }
}


// ---- SETTINGS > RUN DIAGNOSTICS (setup/diagnostics.h)
void DiagnosticsPanel(ImDrawList* dl, const Layout& L, float y) {
    const auto& variants = diag::Variants();
    const float x0 = L.x0, x1 = L.x1;
    {
        Font f(g_head, 20);
        Spaced(dl, ImVec2(x0, y), col::yellow, "RUN DIAGNOSTICS", U(2));
    }
    y += U(34);
    const bool running = g.diag.Running();
    if (running) {
        const int cur = g.diag.current, total = g.diag.total;
        const double el = g.diag.run_elapsed;
        std::string label, status;
        {
            std::lock_guard lk(g.diag.mutex);
            status = g.diag.status;
        }
        if (cur >= 1 && cur <= int(variants.size()))
            for (size_t i = 0, n = 0; i < variants.size(); i++)
                if (g.diag_pick[i] && int(++n) == cur)
                    label = variants[i].label;
        {
            Font f(g_head, 24);
            dl->AddText(ImVec2(x0, y), col::text, fmt::format("TEST {} OF {}  ·  {}", cur, total, label).c_str());
        }
        y += U(40);
        const float frac = float(std::clamp(el / diag::kScriptSeconds, 0.0, 1.0));
        dl->AddRectFilled(ImVec2(x0, y), ImVec2(x1, y + U(10)), IM_COL32(40, 54, 96, 235), U(5));
        dl->AddRectFilled(ImVec2(x0, y), ImVec2(x0 + (x1 - x0) * frac, y + U(10)), col::yellow, U(5));
        y += U(22);
        {
            Font f(g_body, 15);
            const int left = int(std::max(0.0, diag::kScriptSeconds - el)) + int(diag::kScriptSeconds) * (total - cur);
            dl->AddText(ImVec2(x0, y), col::dim,
                        fmt::format("{}  ·  about {}:{:02} left in all", status, left / 60, left % 60).c_str());
            y += U(30);
            dl->AddText(nullptr, 0, ImVec2(x0, y), col::faint,
                        "The game plays by itself. Leave the mouse, keyboard and controller alone until it ends; "
                        "the launcher comes back with the result.",
                        nullptr, x1 - x0);
        }
        y += U(64);
        if (BigButton(dl, "dcancel", "CANCEL", ImVec2(x0, y), ImVec2(x0 + U(220), y + U(50)), g.in_section) ||
            (g.in_section && NavAccept()))
            g.diag.Cancel();
        return;
    }
    if (g.diag_shown_result) {
        std::lock_guard lk(g.diag.mutex);
        int passed = 0;
        for (auto& r : g.diag.results)
            passed += r.pass;
        {
            Font f(g_head, 24);
            dl->AddText(ImVec2(x0, y), passed == int(g.diag.results.size()) ? col::green : col::red,
                        fmt::format("{} OF {} PASSED", passed, g.diag.results.size()).c_str());
        }
        y += U(40);
        for (auto& r : g.diag.results) {
            {
                Font f(g_head, 16);
                dl->AddText(ImVec2(x0, y), r.pass ? col::green : col::red, r.pass ? "PASS" : "FAIL");
                dl->AddText(ImVec2(x0 + U(64), y), col::text, r.label.c_str());
            }
            y += U(22);
            std::string sub = r.slope_known ? fmt::format("memory {:+.0f} MB/min", r.slope) : std::string("memory: no reading");
            if (r.frames)
                sub += fmt::format("  ·  {:.1f} ms average, {:.0f} ms worst", r.avg_ms, r.worst_ms);
            if (!r.why.empty())
                sub += "  ·  " + r.why;
            Font f(g_body, 13.5f);
            dl->AddText(nullptr, 0, ImVec2(x0 + U(64), y), r.pass ? col::dim : col::amber, sub.c_str(), nullptr, x1 - x0 - U(64));
            y += U(34);
        }
        y += U(8);
        {
            Font f(g_body, 14);
            dl->AddText(nullptr, 0, ImVec2(x0, y), col::dim,
                        "The report and the logs are in the folder. To send them, open a new issue on GitHub and attach "
                        "report.txt and the logs. Nothing is sent by the launcher.",
                        nullptr, x1 - x0);
        }
        y += U(56);
        const float bw = (x1 - x0 - U(24)) / 3;
        const char* labels[] = {"OPEN FOLDER", "REPORT IT >", "AGAIN"};
        for (int i = 0; i < 3; i++) {
            const ImVec2 a(x0 + i * (bw + U(12)), y), b(a.x + bw, y + U(46));
            if (BigButton(dl, labels[i], labels[i], a, b, g.in_section && g.diag_sel == i) ||
                (g.in_section && g.diag_sel == i && NavAccept())) {
                if (i == 0)
                    ShellExecuteW(nullptr, L"open", L"explorer.exe",
                                  (L"/select,\"" + (g.diag.folder / "report.txt").wstring() + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
                else if (i == 1) // (only opens the page: the player attaches the files and submits)
                    ShellExecuteW(nullptr, L"open", L"https://github.com/KaikoClanworth1/mua3-native/issues/new", nullptr, nullptr,
                                  SW_SHOWNORMAL);
                else
                    g.diag_shown_result = false, g.diag_sel = 0;
            }
        }
        if (g.in_section) {
            if (Pressed({ImGuiKey_LeftArrow, ImGuiKey_GamepadDpadLeft}))
                g.diag_sel = std::max(0, g.diag_sel - 1);
            if (Pressed({ImGuiKey_RightArrow, ImGuiKey_GamepadDpadRight}))
                g.diag_sel = std::min(2, g.diag_sel + 1);
        }
        return;
    }
    {
        Font f(g_body, 14.5f);
        dl->AddText(nullptr, 0, ImVec2(x0, y), col::dim,
                    "The game plays itself for about 6 minutes per test, with your settings, from a separate empty save "
                    "(your saves are not touched). Afterwards you get a report to attach to a GitHub issue.",
                    nullptr, x1 - x0);
    }
    y += U(62);
    const int n = int(variants.size());
    int picked = 0;
    for (int i = 0; i < n; i++) {
        const ImVec2 a(x0, y), b(x1, y + U(52));
        const bool sel = g.in_section && g.diag_sel == i;
        dl->AddRectFilled(a, b, sel ? IM_COL32(40, 54, 96, 235) : IM_COL32(20, 34, 70, 200));
        if (sel)
            dl->AddRectFilled(a, ImVec2(a.x + U(6), b.y), col::yellow);
        ImGui::PushID(i);
        ImGui::SetCursorScreenPos(a);
        bool flip = ImGui::InvisibleButton("row", b - a);
        bool on = g.diag_pick[size_t(i)];
        flip = Toggle(dl, "##t", ImVec2(a.x + U(18), a.y + U(15)), on) || flip;
        ImGui::PopID();
        {
            Font f(g_head, 16);
            dl->AddText(ImVec2(a.x + U(76), a.y + U(7)), sel ? col::yellow : col::text, variants[size_t(i)].label);
        }
        {
            Font f(g_body, 13);
            dl->AddText(ImVec2(a.x + U(76), a.y + U(29)), col::dim, variants[size_t(i)].sub);
        }
        if (sel && NavAccept())
            flip = true;
        if (flip)
            g.diag_pick[size_t(i)] = !on, g.diag_sel = i;
        picked += g.diag_pick[size_t(i)];
        y += U(58);
    }
    y += U(10);
    const bool can = picked > 0 && !g.game && fs::exists(g.dir / "mua3.exe");
    {
        Font f(g_body, 14);
        dl->AddText(ImVec2(x0 + U(240), y + U(15)), can ? col::dim : col::amber,
                    g.game         ? "Close the game first."
                    : picked == 0 ? "Pick at least one test."
                                  : fmt::format("About {} minutes.", picked * 7).c_str());
    }
    const bool start_sel = g.in_section && g.diag_sel == n;
    if ((BigButton(dl, "dstart", "START", ImVec2(x0, y), ImVec2(x0 + U(220), y + U(50)), start_sel) ||
         (start_sel && NavAccept())) &&
        can) {
        std::vector<int> v;
        for (int i = 0; i < n; i++)
            if (g.diag_pick[size_t(i)])
                v.push_back(i);
        if (g.diag.Start(g.dir, g.host->version, v)) {
            g.diag_shown_result = true; // (shown once it ends)
            if (g.host->window)
                SDL_MinimizeWindow(g.host->window);
        }
    }
    if (g.in_section) {
        if (NavUp())
            g.diag_sel = std::max(0, g.diag_sel - 1);
        if (NavDown())
            g.diag_sel = std::min(n, g.diag_sel + 1);
    }
}

// ---- SETTINGS (the rows come with the shared settings table; until then: the SYSTEM entries)
Action SettingsSection(ImDrawList* dl, const Layout& L) {
    Action act = Action::None;
    float y = L.y0;
    {
        Font f(g_head, 40);
        Shadowed(dl, ImVec2(L.x0, y), col::yellow, "SETTINGS");
    }
    y += U(70);
    if (g.diag_open || g.diag.Running()) {
        DiagnosticsPanel(dl, L, y);
        return act;
    }
    {
        Font f(g_body, 15);
        dl->AddText(nullptr, 0, ImVec2(L.x0, y), col::dim,
                    "The game's own options are in the game: OPTIONS > GRAPHICS and PC SETTINGS. The same settings come here "
                    "next.",
                    nullptr, L.x1 - L.x0);
    }
    y += U(56);
    struct Entry {
        const char* label;
        const char* sub;
        Action action;
    };
    const Entry all[] = {
        {"UPDATE OR REPAIR", "Add an update or DLC, change the language, or build the game again.", Action::UpdateRepair},
        {"REPORT A PROBLEM", "Makes a zip with the logs and crash files to send.", Action::Report},
        {"RUN DIAGNOSTICS", "A 6-minute test run with a report to attach to a bug report.", Action::None},
        {"UNINSTALL", "Remove the game from this PC. You can keep your saves.", Action::Uninstall},
    };
    // a developer's folder (dev=1 in setup.ini) is never rebuilt or removed from here
    Entry entries[4];
    int kEntries = 0;
    for (const Entry& e : all)
        if (!g.host->installed.dev || (e.action != Action::UpdateRepair && e.action != Action::Uninstall))
            entries[kEntries++] = e;
    g.settings_sel = std::min(g.settings_sel, kEntries - 1);
    g.settings_rows = kEntries;
    for (int i = 0; i < kEntries; i++)
        g.settings_labels[i] = entries[i].label;
    for (int i = 0; i < kEntries; i++) {
        const ImVec2 a(L.x0, y), b(L.x1, y + U(62));
        ImGui::SetCursorScreenPos(a);
        ImGui::PushID(i);
        const bool click = ImGui::InvisibleButton("e", b - a);
        const bool hov = ImGui::IsItemHovered();
        ImGui::PopID();
        const bool sel = (g.in_section && g.settings_sel == i) || hov;
        Slant(dl, a, b, U(10), sel ? IM_COL32(40, 54, 96, 235) : IM_COL32(20, 34, 70, 200), sel ? IM_COL32(40, 54, 96, 235) : IM_COL32(20, 34, 70, 200));
        if (sel)
            Bar(dl, a, ImVec2(a.x + U(8), b.y), 0, col::yellow, col::yellow);
        {
            Font f(g_head, 20);
            dl->AddText(ImVec2(a.x + U(26), a.y + U(10)), sel ? col::yellow : col::text, entries[i].label);
        }
        {
            Font f(g_body, 13.5f);
            dl->AddText(ImVec2(a.x + U(26), a.y + U(38)), col::dim, entries[i].sub);
        }
        if (click || (g.in_section && g.settings_sel == i && NavAccept())) {
            act = entries[i].action;
            if (act == Action::None) { // RUN DIAGNOSTICS
                g.diag_open = true;
                g.diag_sel = 0;
                g.in_section = true;
            }
        }
        y += U(70);
    }
    if (g.in_section) {
        if (NavUp())
            g.settings_sel = std::max(0, g.settings_sel - 1);
        if (NavDown())
            g.settings_sel = std::min(kEntries - 1, g.settings_sel + 1);
    }
    return act;
}

void ExitSection(ImDrawList* dl, const Layout& L) {
    const ImVec2 a(L.x1 - U(520), L.y0 + U(56)), b(L.x1, L.y0 + U(56) + U(150));
    Card(dl, a, b);
    {
        Font f(g_head, 30);
        dl->AddText(ImVec2(a.x + U(34), a.y + U(28)), col::yellow, "EXIT");
    }
    Font f(g_body, 15);
    dl->AddText(ImVec2(a.x + U(34), a.y + U(74)), col::dim, "Close the launcher. Your settings and mods are saved.");
}

ImFont* LoadResourceFont(int id) {
    HRSRC r = FindResourceW(nullptr, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(10)); // RT_RCDATA
    if (!r)
        return nullptr;
    HGLOBAL h = LoadResource(nullptr, r);
    void* data = h ? LockResource(h) : nullptr;
    const DWORD size = SizeofResource(nullptr, r);
    if (!data || !size)
        return nullptr;
    ImFontConfig fc;
    fc.FontDataOwnedByAtlas = false; // (the exe's own memory)
    return ImGui::GetIO().Fonts->AddFontFromMemoryTTF(data, int(size), 18.0f, &fc);
}

} // namespace

void LoadFonts() {
    g_head = LoadResourceFont(201);
    g_body = LoadResourceFont(202);
    if (!g_head || !g_body) {
        LOG_WARN("Launcher", "Exo 2 not in the resources: the default font is used");
        g_head = g_body = ImGui::GetIO().Fonts->Fonts.empty() ? ImGui::GetIO().Fonts->AddFontDefault() : ImGui::GetIO().Fonts->Fonts[0];
    }
}

void Open(Host& host) {
    g.host = &host;
    g.dir = host.installed.dir;
    if (g_stars.empty())
        MakeStars();
    g.cfg = core::Config();
    g.cfg.Load(Ini());
    g.mods_on = g.cfg.mods_enabled;
    ScanMods();
    ProbeGame();
    if (host.installed.dev) // (no UPDATE NOW: an update rebuilds the folder)
        LOG_INFO("Launcher", "{}: developer folder (dev=1), no update check", g.dir.string());
    else
        g.update.Check(host.version);
}

Action Frame(Host& host) {
    g.host = &host;
    CheckGame();
    ImGuiIO& io = ImGui::GetIO();
    g.t += io.DeltaTime;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    Layout L;
    L.W = vp->Size.x, L.H = vp->Size.y;
    g_u = std::min(L.W / 1280.0f, L.H / 720.0f);
    L.panel_top = std::max(L.W * 0.406f, U(470));
    L.panel_bottom = L.panel_top - U(115);
    L.x0 = L.panel_top + U(84);
    L.x1 = L.W - U(48);
    L.y0 = U(56);
    L.y1 = L.H - U(56) - U(16);

    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("launcher", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBackground |
                                          ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollbar |
                                          ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    Background(dl, L);
    Title(dl);
    Action act = Action::None;
    const int activated = Menu(dl, L);
    const bool modal = g.confirm_remove >= 0;
    if (activated == kExit)
        act = Action::Quit;
    else if (activated == kMods || activated == kSettings)
        g.in_section = true;
    if (g.in_section && !modal && NavBack()) {
        if (g.sel == kSettings && g.diag_open && !g.diag.Running())
            g.diag_open = false;
        else
            g.in_section = false;
    }
    {
        static bool was_running = false;
        const bool r = g.diag.Running();
        if (was_running && !r && host.window) {
            SDL_RestoreWindow(host.window);
            SDL_RaiseWindow(host.window);
            g.sel = kSettings, g.in_section = true, g.diag_open = true, g.diag_sel = 0;
            g.cfg.Load(Ini());
        }
        was_running = r;
    }
    switch (g.sel) {
    case kPlay: PlaySection(dl, L, activated == kPlay); break;
    case kMods: ModsSection(dl, L); break;
    case kSettings:
        if (const Action a = SettingsSection(dl, L); a != Action::None)
            act = a;
        break;
    case kExit: ExitSection(dl, L); break;
    }
    if (g.update.state == updater::Updater::State::Available && !g.update_started && std::getenv("MUA3_UPDATE_START") &&
        !ManagedElsewhere()) { // (developer test: no click)
        g.update_started = true;
        g.update.Start(host.installed.source_dir);
    }
    if (g.update.state == updater::Updater::State::Ready && !g.game && g.update.Run(host.installed.dir))
        act = Action::Quit; // the new setup takes over (it replaces this launcher)
    if (g.ended == "!report") {
        g.ended.clear();
        act = Action::Report;
    }
    // the footer: version, button prompts
    dl->AddRectFilled(ImVec2(0, L.H - U(56)), ImVec2(L.W, L.H), IM_COL32(0, 0, 0, 140));
    {
        Font f(g_body, 12);
        std::string v = "MUA3 Native " + host.version;
        if (g.update.state == updater::Updater::State::Available) {
            std::lock_guard lk(g.update.mutex);
            v += "  ·  " + g.update.release.version + " available";
        }
        dl->AddText(ImVec2(U(40), L.H - U(36)), IM_COL32(125, 141, 176, 255), v.c_str());
    }
    float right = L.W - U(40);
    const float fy = L.H - U(28);
    if (g.in_section && g.sel == kMods) {
        right = Prompt(dl, right, fy, "B", "BACK");
        right = Prompt(dl, right, fy, "Y", "HOLD + MOVE");
        right = Prompt(dl, right, fy, "A", "ON / OFF");
    } else if (g.in_section) {
        right = Prompt(dl, right, fy, "B", "BACK");
        right = Prompt(dl, right, fy, "A", "SELECT");
    } else {
        right = Prompt(dl, right, fy, "A", "SELECT");
    }
    ImGui::End();
    return act;
}

void Drop(const fs::path& path) {
    g.sel = kMods;
    g.in_section = true;
    StartInstall(path);
}

bool GameRunning() { return g.game != nullptr || g.diag.Running(); }

void Shutdown() {
    g.diag.Stop(); // (ends a run it started and waits for its report)
    if (g.probe.joinable())
        g.probe.join();
    if (g.analysis.joinable())
        g.analysis.join();
    if (g.install.joinable())
        g.install.join();
    for (auto& r : g.mods)
        ui::Release(r.tex);
    if (g.game)
        CloseHandle(g.game), g.game = nullptr;
}

void Select(int section, bool focus) {
    g.sel = std::clamp(section, 0, kSections - 1);
    g.in_section = focus;
}

bool TestPlay() { return !g.game && Launch(); }

void TestToggleMods() {
    g.mods_on = !g.mods_on;
    SaveMods(g.mods_on ? "Mods are on." : "Mods are off: the game starts without them.");
}

std::string SettingsRows() {
    std::string s;
    for (int i = 0; i < g.settings_rows; i++)
        s += (i ? ", " : "") + std::string(g.settings_labels[i]);
    return s;
}

void OpenDiagnostics(const fs::path& sample_log) {
    g.sel = kSettings;
    g.in_section = true;
    g.diag_open = true;
    g.diag_sel = 0;
    if (sample_log.empty())
        return;
    std::lock_guard lk(g.diag.mutex);
    g.diag.results.clear();
    for (size_t i = 0; i < 2; i++) {
        diag::RunResult r;
        r.id = diag::Variants()[i].id, r.label = diag::Variants()[i].label;
        r.finished = true;
        diag::Analyze(sample_log, r);
        LOG_INFO("Launcher", "diagnostics sample: {} {} slope {:.1f} known {} frames {} avg {:.2f} worst {:.1f} late {} errors {} why '{}'",
                 r.label, r.pass ? "PASS" : "FAIL", r.slope, r.slope_known, r.frames, r.avg_ms, r.worst_ms, r.late, r.errors, r.why);
        g.diag.results.push_back(r);
    }
    g.diag.folder = g.dir / "diag" / "sample";
    g.diag_shown_result = true;
}

std::string InstallNow(const fs::path& path) {
    bool ok = false;
    const std::string msg = InstallMod(path, ok);
    ScanMods();
    SelectInstalled();
    g.mods_note = msg;
    g.mods_note_col = ok ? col::green : col::red;
    return msg;
}

} // namespace launcher
