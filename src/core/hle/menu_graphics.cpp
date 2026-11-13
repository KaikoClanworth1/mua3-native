// The in-game GRAPHICS page: the game's OPTIONS screen (09_option_* layouts), showing the PC display settings.
//
// How the OPTIONS screen works (main module, 4.0.1; class vtable main+0x2121e40, notes in docs/game_menus.md):
//  - Five tabs (INTERFACE, CAMERA, CONTROLS, SOUND, OTHER; current tab at this+0x198), each a fixed list of rows.
//    Per tab: a row count (u16[5] at main+0x1a634b2), row types (u32 arrays, tab 2 at main+0x1a634ec: 0 button,
//    1 two choices, 2 arrows, 3 gauge), a row table reached through a GOT entry ({u32 label hash, u32 info hash,
//    u64 choices} with choices a u32 array of label hashes; tab 2 at main+0x218bd40) and the current values, one
//    byte per row, also through a GOT entry (tab 2 at main+0x218bd20).
//  - Opening the screen (vfunc +0x3f8, main+0x7c5050) copies the game's settings into the value bytes and shows
//    tab 0; closing it (vfunc +0x400) copies them back (main+0x7c60a0). Left / right on a row (vfuncs +0x1b8 /
//    +0x1c0) step the row's byte; an arrow row wraps at ChoiceCount(this, row) (main+0x7c91f0). L / R switch
//    tabs (vfuncs +0x1e8 / +0x1f0), A runs a button row (vfunc +0x228). All text comes from the system string
//    table by hash (GetString, main+0x1d6290).
//
// Our pages (GRAPHICS and PC SETTINGS) are tab 2 with its data replaced while they are open: our row table, row
// types, value bytes and choice counts, our text for hashes 0xFE00xxxx. Only the headers of tabs 2 and 3 show,
// named GRAPHICS (the game's own string) and PC SETTINGS; L / R switch between the two pages (tab 2's data slot
// is refilled, tab 3's header lights up), A steps a row to its next value. Every change is applied at once
// (core::System::UpdateConfig): what can change while the game runs changes now, the rest is saved to mua3.ini for
// the next start and the row says so. Closing puts the tab back and leaves the game's own settings as they were.
//
// The way in: the CONTROLS tab (tab 2) of every OPTIONS screen ends with a GRAPHICS row (a button row: tab 2's row
// table gets a 4th entry, its 4th row type is already 0). A on it turns the screen into our pages in place; B on our
// pages comes back to CONTROLS. The title screen's GRAPHICS item opens an OPTIONS screen straight on our pages
// (OpenNext), and there B closes the screen.
#include "core/hle/menu_graphics.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <chrono>
#include <mutex>
#include <thread>
#include <string>
#include <tuple>
#include <vector>

#include "core/hle/menu_guest.h"
#include "core/kernel/kernel.h"
#include "core/memory.h"

namespace hle::menu::graphics {
namespace {

using recomp::State;

// ---- the game's code and data (offsets into main, 4.0.1)
constexpr u64 kOptInit = 0x7c5050;                    // vfunc +0x3f8 (this, a, b): settings -> value bytes, tab 0
constexpr u64 kOptStore = 0x7c60a0;                   // (this): value bytes -> the game's settings (direct)
constexpr u64 kChoiceCount = 0x7c91f0;                // (this, row) -> choices of an arrow row (direct)
constexpr u64 kTabPrev = 0x7c81e0, kTabNext = 0x7c8320; // vfuncs +0x1e8 / +0x1f0 (this): L / R
constexpr u64 kValuePrev = 0x7c8480, kValueNext = 0x7c9400; // vfuncs +0x1b8 / +0x1c0 (this): left / right
constexpr u64 kDecide = 0x7c7c10;                     // vfunc +0x228 (this): A on a row
constexpr u64 kDefault = 0x7c7eb0;                    // vfunc +0x1d8 (this): Y, asks to reset the row
constexpr u64 kShowValue = 0x7c8970;                  // (this, row): draws a row's value
constexpr u64 kFillRows = 0x7c5900;                   // (this): lays out the current tab's rows
constexpr u64 kRowsDone = 0x7c5d80;                   // (this): what opening does after laying out the rows
constexpr u64 kBack = 0x7c7e40;                       // vfunc +0x230 (this): B, closes the screen
constexpr u64 kTabNames = 0x7c5370;                   // (this): the tab headers' texts (GetString)
constexpr u64 kPlayAnim = 0x5e4d00;                   // (animation helper, animation, 0)
constexpr u64 kPane = 0x5f39d0;                       // (layout, pane index) -> pane
constexpr u64 kSetVisible = 0x12f5d40;                // (pane, visible)
constexpr u64 kGetString = 0x1d6290;                  // (string table, hash) -> const char16_t* (direct)
constexpr u64 kRowsGot = 0x218bd40, kValuesGot = 0x218bd20; // tab 2's row table and value bytes (GOT entries)
constexpr u64 kRowCounts = 0x1a634b2;                 // u16[5]
constexpr u64 kRowTypes = 0x1a634ec;                  // u32[] tab 2 (the next table, the language row's, is tab 0's)
constexpr u64 kTabAnims = 0x1a634a8, kTabOffAnims = 0x1a634ad; // u8[5]: tab_0N_ON / tab_0N_OFF animations
constexpr u64 kPlaySound = 0x563690, kSoundGot = 0x2188cc0; // (sound player, id): what L / R play
constexpr u32 kSoundTab = 0xba0cf4d2, kSoundBack = 0x60d8ae5e;
constexpr u32 kTab = 2;                                 // the tab whose data slot shows our pages
constexpr u32 kPageTabs[2] = {2, 3};                    // the header each page lights up
// options object layout
constexpr u64 kObjTab = 0x198, kObjState = 0x1a0, kObjAnims = 0xc0, kObjTabLayout = 0xc8;
constexpr u32 kStateIdle = 8;
// 09_option_tub_menu panes: tab N (0-4) = Window_tab_0N, Window_tab_ON_0N, tab_name_0N at 1 + 3N..; L/R icons 16-19
constexpr u32 kTabPanes = 1, kPanesPerTab = 3, kButtonPanes = 16, kButtonPaneCount = 4;
// system string hashes
constexpr u32 kStrControls = 0x023fd6, kStrSound = 0x1f0d31, kStrGraphics = 0x36c4b2, kStrOn = 0x3a32fb,
              kStrOff = 0x19edbe, kStrAuto = 0x0855d9;
constexpr u32 kOurs = 0xFE000000; // our strings: kOurs + index (the game's hashes are 22-bit)

enum RowType : u32 { kTwoChoices = 1, kArrows = 2 };

constexpr std::pair<int, int> kResolutions[] = {{1280, 720},  {1600, 900},  {1920, 1080}, {2560, 1440}, {3200, 1800},
                                                {3840, 2160}, {2560, 1080}, {3440, 1440}, {5120, 2880}};
constexpr int kRates[] = {30, 60, 72, 90, 120, 144, 165, 240}; // as the launcher offers them
constexpr int kLimits[] = {0, 30, 60, 72, 90, 120, 144, 165, 240};

constexpr u32 kMaxRows = 8; // tab 2's row-type table has room for 8 (the next table is tab 0's)
enum GraphicsRows { kRowMode, kRowResolution, kRowScale, kRowRate, kRowLimit, kRowVsync, kRowShowFps, kRowAa, kGraphicsRows };
enum PcRows { kRowVolume, kRowMute, kRowCamera, kRowController, kRowAudio, kPcRows };
static_assert(kGraphicsRows <= kMaxRows && kPcRows <= kMaxRows);
constexpr int kCameraSpeeds[] = {10, 20, 30, 40, 50, 60, 70, 80, 90, 100, 110, 120, 130, 140, 150, 160, 170, 180, 190, 200};
const char16_t* const kControllerNames[] = {u"XBOX (XINPUT)", u"ANY CONTROLLER (SDL)"};
const char16_t* const kAudioNames[] = {u"XAUDIO2", u"SDL", u"OFF"};

core::Memory* g_mem = nullptr;
u64 g_main = 0;

// guest memory: strings, then the row table, choice arrays and value bytes
u64 g_block = 0, g_alloc = 0;
constexpr u64 kBlockSize = 0x8000;
std::vector<u64> g_strings;     // our string index -> guest address
u64 g_custom_res_text = 0;      // "W x H" for a window size that is no preset (rewritten on open)
u64 g_custom_limit_text = 0;    // "N" for an fps limit that is no preset
struct RowInfo {
    u32 label, info;
    RowType type;
    std::vector<u32> choices; // hashes
    u64 guest_choices = 0;
};
struct Page {
    u32 title = 0; // the header's text (string hash)
    std::vector<RowInfo> rows;
    u64 rows_guest = 0, values = 0;
    u8 applied[kMaxRows]{}; // the value bytes last applied
    void (*to)(const core::Config&, u8* v) = nullptr;
    void (*from)(core::Config&, bool running, const u8* v) = nullptr;
};
Page g_pages[2];
int g_cur = 0; // the page shown
Page& Cur() { return g_pages[g_cur]; }

std::mutex g_mutex;
std::atomic<bool> g_open_next{false}, g_active{false};
u64 g_page = 0; // the options object showing the page
bool g_from_options = false; // the page was opened from the GRAPHICS row: B goes back to CONTROLS
u64 g_saved_rows = 0, g_saved_values = 0;
constexpr u32 kEntryRow = 3; // CONTROLS' GRAPHICS row
u32 g_entry_info = 0;        // its info line
u64 g_entry_rows = 0;        // CONTROLS' row table with the GRAPHICS row (guest)
u16 g_saved_count = 0;
u32 g_saved_types[kMaxRows]{};

enum Hook {
    kHInit, kHStore, kHCount, kHTabPrev, kHTabNext, kHDecide, kHDefault, kHString, kHBack, kHookCount
};
cpu::FunctionOverride g_hooks[kHookCount];

u64 R64(u64 a) { return g_mem->Read<u64>(a); }
u32 R32(u64 a) { return g_mem->Read<u32>(a); }
void W64(u64 a, u64 v) { g_mem->Write<u64>(a, v); }
void W32(u64 a, u32 v) { g_mem->Write<u32>(a, v); }

u64 Alloc(u64 size) {
    const u64 a = g_alloc;
    g_alloc = (g_alloc + size + 15) & ~15ull;
    if (g_alloc > g_block + kBlockSize)
        LOG_ERROR("Menu", "graphics page: out of guest memory");
    return a;
}
u64 PutText(const std::u16string& t, size_t room = 0) {
    const u64 a = Alloc(std::max(t.size() + 1, room) * 2);
    g_mem->WriteBlock(a, t.c_str(), (t.size() + 1) * 2);
    return a;
}
// A string of ours: its hash.
u32 Text(const std::u16string& t, size_t room = 0) {
    g_strings.push_back(PutText(t, room));
    return kOurs + u32(g_strings.size() - 1);
}
std::u16string U16(const std::string& s) { return std::u16string(s.begin(), s.end()); }

u32 g_custom_res = 0, g_custom_limit = 0, g_custom_volume = 0, g_custom_camera = 0; // rewritable value texts
// the restart rows' info lines (they name the value running now)
u32 g_scale_info = 0, g_rate_info = 0, g_controller_info = 0, g_audio_info = 0;
const std::u16string kScaleInfo = u"The resolution the game renders at. AUTO matches your display.";
const std::u16string kRateInfo = u"The game's frame rate. 30 is the original, 60 is fully supported.";
const std::u16string kControllerInfo = u"XINPUT: Xbox controllers. SDL: PlayStation, Switch Pro and other controllers.";
const std::u16string kAudioInfo = u"How the game's sound is played. XAUDIO2 suits most PCs; OFF plays no sound.";
const std::u16string kScaleNames[] = {u"AUTO", u"1X (1080P)", u"2X (1440P / 4K)", u"3X", u"4X"};
constexpr size_t kInfoRoom = 200;
std::atomic<u32> g_scale3{0}; // the running render scale in thirds (0 = not told)

// A page's row table, choice arrays and value bytes in guest memory.
void Finish(Page& page) {
    for (RowInfo& r : page.rows) {
        r.guest_choices = Alloc(4 * r.choices.size());
        for (size_t i = 0; i < r.choices.size(); i++)
            W32(r.guest_choices + 4 * i, r.choices[i]);
    }
    page.rows_guest = Alloc(16 * page.rows.size());
    for (size_t i = 0; i < page.rows.size(); i++) {
        W32(page.rows_guest + 16 * i, page.rows[i].label);
        W32(page.rows_guest + 16 * i + 4, page.rows[i].info);
        W64(page.rows_guest + 16 * i + 8, page.rows[i].guest_choices);
    }
    page.values = Alloc(16);
}

void ToGraphics(const core::Config& c, u8* v);
void FromGraphics(core::Config& c, bool running, const u8* v);
void ToPc(const core::Config& c, u8* v);
void FromPc(core::Config& c, bool running, const u8* v);

void BuildPages() {
    g_block = g_mem->FindFree(core::layout::kKernelMapBase, 0x1000000000ull, kBlockSize);
    g_mem->Map(g_block, kBlockSize, core::MS_Normal, core::MP_RW);
    g_alloc = g_block;
    Page* page = nullptr;
    auto add = [&page](u32 label, u32 info, RowType type, std::vector<u32> choices) {
        page->rows.push_back({label, info, type, std::move(choices)});
    };
    // GRAPHICS
    page = &g_pages[0];
    page->title = kStrGraphics;
    page->to = ToGraphics;
    page->from = FromGraphics;
    add(Text(u"DISPLAY MODE"), Text(u"Play in a window or fullscreen. F11 also switches."), kTwoChoices,
        {Text(u"WINDOWED"), Text(u"FULLSCREEN")});
    std::vector<u32> res;
    for (auto [w, h] : kResolutions)
        res.push_back(Text(U16(fmt::format("{} x {}", w, h))));
    g_custom_res = Text(u"", 24);
    res.push_back(g_custom_res);
    add(Text(u"RESOLUTION"), Text(u"The window's size in windowed mode.\r\nFullscreen uses the monitor's resolution."),
        kArrows, res);
    g_scale_info = Text(kScaleInfo, kInfoRoom);
    add(Text(u"RENDER SCALE"), g_scale_info, kArrows,
        {kStrAuto, Text(kScaleNames[1]), Text(kScaleNames[2]), Text(kScaleNames[3]), Text(kScaleNames[4])});
    std::vector<u32> rates;
    for (int r : kRates)
        rates.push_back(Text(U16(r == 30 ? "30 (ORIGINAL)" : r > 60 ? fmt::format("{} (EXPERIMENTAL)", r)
                                                                   : std::to_string(r))));
    g_rate_info = Text(kRateInfo, kInfoRoom);
    add(Text(u"FRAME RATE"), g_rate_info, kArrows, rates);
    std::vector<u32> limits;
    for (int l : kLimits)
        limits.push_back(l == 0 ? kStrOff : Text(U16(std::to_string(l))));
    g_custom_limit = Text(u"", 8);
    limits.push_back(g_custom_limit);
    add(Text(u"FPS LIMIT"), Text(u"Caps how fast frames are shown. OFF = no cap."), kArrows, limits);
    add(Text(u"VSYNC"), Text(u"Synchronizes frames with the display to prevent tearing."), kTwoChoices,
        {kStrOn, kStrOff});
    add(Text(u"SHOW FPS"), Text(u"Shows the frame rate in the corner of the screen."), kTwoChoices,
        {kStrOn, kStrOff});
    add(Text(u"ANTI-ALIASING"),
        Text(u"Smooths jagged edges. FXAA costs nothing; SSAA renders more pixels (high VRAM).\r\n"
             u"SSAA multiplies the render scale (4X in total at most)."),
        kArrows, {kStrOff, Text(u"FXAA"), Text(u"SSAA 2X"), Text(u"SSAA 3X"), Text(u"SSAA 4X")});
    Finish(*page);
    // PC SETTINGS
    page = &g_pages[1];
    page->title = Text(u"PC SETTINGS");
    page->to = ToPc;
    page->from = FromPc;
    std::vector<u32> volumes;
    for (int v = 0; v <= 100; v += 10)
        volumes.push_back(Text(U16(fmt::format("{}%", v))));
    g_custom_volume = Text(u"", 8);
    volumes.push_back(g_custom_volume);
    add(Text(u"MASTER VOLUME"), Text(u"The volume of all the game's sound, on top of the SOUND tab's own volumes."),
        kArrows, volumes);
    add(Text(u"MUTE"), Text(u"Silences the game. The volume is kept for when you switch it back off."), kTwoChoices,
        {kStrOn, kStrOff});
    std::vector<u32> speeds;
    for (int v : kCameraSpeeds)
        speeds.push_back(Text(U16(v == 100 ? "100% (ORIGINAL)" : fmt::format("{}%", v))));
    g_custom_camera = Text(u"", 8);
    speeds.push_back(g_custom_camera);
    add(Text(u"CAMERA SPEED"), Text(u"How fast the right stick turns the camera. 100% is the original speed.\r\n"
             u"Multiplies the CAMERA tab's CONTROL SENSITIVITY."), kArrows,
        speeds);
    g_controller_info = Text(kControllerInfo, kInfoRoom);
    add(Text(u"CONTROLLER TYPE"), g_controller_info, kArrows, {Text(kControllerNames[0]), Text(kControllerNames[1])});
    g_audio_info = Text(kAudioInfo, kInfoRoom);
    add(Text(u"AUDIO OUTPUT"), g_audio_info, kArrows,
        {Text(kAudioNames[0]), Text(kAudioNames[1]), Text(kAudioNames[2])});
    Finish(*page);
    g_custom_res_text = g_strings[g_custom_res - kOurs];
    g_custom_limit_text = g_strings[g_custom_limit - kOurs];
}

template <size_t N, typename T> int IndexOf(const T (&list)[N], const T& v, int otherwise) {
    for (size_t i = 0; i < N; i++)
        if (list[i] == v)
            return int(i);
    return otherwise;
}

void PutString(u32 hash, const std::u16string& t, size_t room) {
    const size_t n = std::min(t.size(), room - 1);
    std::u16string u = t.substr(0, n);
    g_mem->WriteBlock(g_strings[hash - kOurs], u.c_str(), (n + 1) * 2);
}

// The settings as value bytes (custom value texts rewritten).
void ToGraphics(const core::Config& c, u8* v) {
    v[kRowMode] = c.fullscreen ? 1 : 0;
    const int res = IndexOf(kResolutions, std::pair{c.width, c.height}, int(std::size(kResolutions)));
    PutString(g_custom_res, U16(fmt::format("{} x {}", c.width, c.height)), 24);
    v[kRowResolution] = u8(res);
    v[kRowScale] = u8(std::clamp(c.resolution_scale, 0, 4));
    v[kRowRate] = u8(IndexOf(kRates, c.fps_target, 1));
    PutString(g_custom_limit, U16(std::to_string(c.fps_limit)), 8);
    v[kRowLimit] = u8(IndexOf(kLimits, c.fps_limit, int(std::size(kLimits))));
    v[kRowVsync] = c.vsync ? 0 : 1;
    v[kRowShowFps] = c.show_fps ? 0 : 1;
    v[kRowAa] = u8(std::clamp(c.anti_aliasing, 0, 4));
}
void ToPc(const core::Config& c, u8* v) {
    PutString(g_custom_volume, U16(fmt::format("{}%", c.volume)), 8);
    v[kRowVolume] = u8(c.volume % 10 == 0 ? std::clamp(c.volume, 0, 100) / 10 : 11);
    v[kRowMute] = c.mute ? 0 : 1;
    PutString(g_custom_camera, U16(fmt::format("{}%", c.camera_speed)), 8);
    v[kRowCamera] = u8(IndexOf(kCameraSpeeds, c.camera_speed, int(std::size(kCameraSpeeds))));
    v[kRowController] = u8(std::clamp(int(c.input), 0, 1));
    v[kRowAudio] = u8(std::clamp(int(c.audio), 0, 2));
}

// The value bytes as settings: `running` = only what can change while the game runs.
void FromGraphics(core::Config& c, bool running, const u8* v) {
    c.fullscreen = v[kRowMode] == 1;
    if (v[kRowResolution] < std::size(kResolutions))
        std::tie(c.width, c.height) = kResolutions[v[kRowResolution]];
    if (v[kRowLimit] < std::size(kLimits))
        c.fps_limit = kLimits[v[kRowLimit]];
    c.vsync = v[kRowVsync] == 0;
    c.show_fps = v[kRowShowFps] == 0;
    c.anti_aliasing = std::min<int>(v[kRowAa], 4);
    if (running)
        return;
    c.resolution_scale = std::min<int>(v[kRowScale], 4);
    if (v[kRowRate] < std::size(kRates))
        c.fps_target = kRates[v[kRowRate]];
}
void FromPc(core::Config& c, bool running, const u8* v) {
    if (v[kRowVolume] <= 10)
        c.volume = v[kRowVolume] * 10;
    c.mute = v[kRowMute] == 0;
    if (v[kRowCamera] < std::size(kCameraSpeeds))
        c.camera_speed = kCameraSpeeds[v[kRowCamera]];
    if (running)
        return;
    c.input = core::InputBackend(std::min<int>(v[kRowController], 1));
    c.audio = core::AudioBackend(std::min<int>(v[kRowAudio], 2));
}

void ToPage(Page& page, const core::Config& c) {
    u8 v[16]{};
    page.to(c, v);
    g_mem->WriteBlock(page.values, v, sizeof(v));
}

// What the page shows: the running settings, and for the rows that apply after a restart, the value saved for the
// next start (mua3.ini).
core::Config PageConfig() {
    core::Config c = core::Sys().CfgSnapshot();
    core::Config disk;
    if (disk.Load("mua3.ini")) {
        c.resolution_scale = disk.resolution_scale;
        c.fps_target = disk.fps_target;
        c.input = disk.input;
        c.audio = disk.audio;
    }
    return c;
}

// The restart rows' info lines name what the game runs with now.
void WriteRestartInfo() {
    const core::Config c = core::Sys().CfgSnapshot();
    auto put = [](u32 hash, const std::u16string& info, const std::u16string& now) {
        PutString(hash, info + u"\r\nApplies after a restart (running now: " + now + u").", kInfoRoom);
    };
    // RENDER SCALE: also the size the game renders at now (the renderer's rule: SSAA multiplies the scale, the
    // product capped at 4x by dropping the factor first)
    std::u16string size;
    if (const u32 s3 = g_scale3) {
        const u32 w = c.docked ? 1920 : 1280, h = c.docked ? 1080 : 720;
        auto dims = [&](u32 s) { return U16(fmt::format("{} x {}", (w * s + 2) / 3, (h * s + 2) / 3)); };
        u32 f = c.anti_aliasing >= 2 && c.anti_aliasing <= 4 ? u32(c.anti_aliasing) : 1;
        while (f > 1 && s3 * f > 12)
            f--;
        size = u"\r\nRenders at " + dims(s3) +
               (f > 1 ? u" (" + dims(s3 * f) + u" with SSAA " + U16(std::to_string(f)) + u"X)" : u"") + u" now.";
    }
    PutString(g_scale_info,
              kScaleInfo + u"\r\nApplies after a restart (running now: " +
                  kScaleNames[std::clamp(c.resolution_scale, 0, 4)] + u")." + size,
              kInfoRoom);
    put(g_rate_info, kRateInfo, U16(std::to_string(c.fps_target)) + u" FPS");
    put(g_controller_info, kControllerInfo, kControllerNames[std::clamp(int(c.input), 0, 1)]);
    put(g_audio_info, kAudioInfo, kAudioNames[std::clamp(int(c.audio), 0, 2)]);
}

// A changed page applies at once.
void Apply(bool always = false) {
    Page& page = Cur();
    u8 v[16];
    g_mem->ReadBlock(page.values, v, sizeof(v));
    if (!always && std::memcmp(v, page.applied, kMaxRows) == 0)
        return;
    std::memcpy(page.applied, v, kMaxRows);
    core::Sys().UpdateConfig([&page, &v](core::Config& c, bool running) { page.from(c, running, v); });
    WriteRestartInfo(); // the render size follows the anti-aliasing
}

// The current page into tab 2's data slot (g_mutex held).
void Show(Page& page) {
    W64(g_main + kRowsGot, page.rows_guest);
    W64(g_main + kValuesGot, page.values);
    g_mem->Write<u16>(g_main + kRowCounts + 2 * kTab, u16(page.rows.size()));
    for (u32 i = 0; i < page.rows.size(); i++)
        W32(g_main + kRowTypes + 4 * i, page.rows[i].type);
    ToPage(page, PageConfig());
    WriteRestartInfo();
    g_mem->ReadBlock(page.values, page.applied, kMaxRows);
}

// ---- hooks
void Hide(State& s, u64 layout, u32 pane) {
    if (const u64 p = layout ? GuestCall(s, g_main + kPane, {layout, pane}) : 0)
        GuestCall(s, g_main + kSetVisible, {p, 0});
}

// Only the two headers of our pages show, and the L / R icons.
void HideOthers(State& s, u64 opt) {
    const u64 tabs = R64(opt + kObjTabLayout);
    for (u32 t = 0; t < 5; t++)
        if (t != kPageTabs[0] && t != kPageTabs[1])
            for (u32 p = 0; p < kPanesPerTab; p++)
                Hide(s, tabs, kTabPanes + kPanesPerTab * t + p);
}

// Our pages in the screen's tab 2 slot (the screen is open; from_options: from the GRAPHICS row).
void EnterPage(State& s, u64 opt, bool from_options) {
    {
        std::lock_guard lk(g_mutex);
        if (g_active) // not closed the usual way: the tables still hold the page
            return;
        g_saved_rows = R64(g_main + kRowsGot);
        g_saved_values = R64(g_main + kValuesGot);
        g_saved_count = g_mem->Read<u16>(g_main + kRowCounts + 2 * kTab);
        for (u32 i = 0; i < kMaxRows; i++)
            g_saved_types[i] = R32(g_main + kRowTypes + 4 * i);
        g_cur = 0;
        Show(Cur());
        g_page = opt;
        g_from_options = from_options;
        g_active = true;
    }
    // show tab 2 (named GRAPHICS by the string hook) next to tab 3 (PC SETTINGS)
    g_mem->Write<u32>(opt + kObjTab, kTab);
    if (from_options) { // the headers were named at opening: again, now as our pages
        GuestCall(s, g_main + kTabNames, {opt});
        if (const u64 sound = R64(g_main + kSoundGot))
            GuestCall(s, g_main + kPlaySound, {sound + 0x168, kSoundTab});
    }
    GuestCall(s, g_main + kPlayAnim, {opt + kObjAnims, g_mem->Read<u8>(g_main + kTabAnims + kPageTabs[0]), 0});
    GuestCall(s, g_main + kFillRows, {opt});
    GuestCall(s, g_main + kRowsDone, {opt});
    HideOthers(s, opt);
    LOG_INFO("Menu", "Graphics page opened{}", from_options ? " from OPTIONS" : "");
}

// Our pages' data out of tab 2's slot again (g_mutex held).
void PutTabBack() {
    Apply(true);
    W64(g_main + kRowsGot, g_saved_rows);
    W64(g_main + kValuesGot, g_saved_values);
    g_mem->Write<u16>(g_main + kRowCounts + 2 * kTab, g_saved_count);
    for (u32 i = 0; i < kMaxRows; i++)
        W32(g_main + kRowTypes + 4 * i, g_saved_types[i]);
    g_active = false;
    g_page = 0;
    g_from_options = false;
}

// CONTROLS gets the GRAPHICS row (its row type, 0 = button, is already there): its row table with one more entry
// and a row count of 4, once the screen is open (the opening sees the game's 3 rows
// and closing puts them back). g_mutex held, our pages not in tab 2's slot.
u64 g_game_rows = 0; // CONTROLS' own row table
void RemoveEntryRow() {
    if (g_entry_rows && R64(g_main + kRowsGot) == g_entry_rows) {
        W64(g_main + kRowsGot, g_game_rows);
        g_mem->Write<u16>(g_main + kRowCounts + 2 * kTab, u16(kEntryRow));
    }
}
void AddEntryRow() {
    const u64 rows = R64(g_main + kRowsGot);
    const u16 n = g_mem->Read<u16>(g_main + kRowCounts + 2 * kTab);
    if (rows == g_entry_rows && n == kEntryRow + 1)
        return;
    if (n != kEntryRow || !rows) {
        LOG_WARN("Menu", "CONTROLS has {} rows ({:#x}): no GRAPHICS row", n, rows);
        return;
    }
    g_game_rows = rows;
    if (!g_entry_rows) {
        g_entry_rows = Alloc(16 * (kEntryRow + 1));
        u8 copy[16 * kEntryRow];
        g_mem->ReadBlock(rows, copy, sizeof(copy));
        g_mem->WriteBlock(g_entry_rows, copy, sizeof(copy));
        W32(g_entry_rows + 16 * kEntryRow, kStrGraphics);
        W32(g_entry_rows + 16 * kEntryRow + 4, g_entry_info);
        // a button row shows its one choice (BUTTON SETTINGS' CONFIGURE): the same
        W64(g_entry_rows + 16 * kEntryRow + 8, R64(rows + 16 * (kEntryRow - 1) + 8));
        LOG_INFO("Menu", "GRAPHICS row added to OPTIONS > CONTROLS");
    }
    W64(g_main + kRowsGot, g_entry_rows);
    g_mem->Write<u16>(g_main + kRowCounts + 2 * kTab, u16(kEntryRow + 1));
}

// Opening the OPTIONS screen: as the GRAPHICS page if asked for.
void OptInit(State& s) {
    const u64 opt = s.x[0];
    {
        std::lock_guard lk(g_mutex);
        if (!g_active)
            RemoveEntryRow(); // a screen closed without the store: the game's table again
    }
    RunOriginal(s, g_hooks[kHInit]);
    {
        std::lock_guard lk(g_mutex);
        if (!g_active)
            AddEntryRow();
    }
    if (g_open_next.exchange(false))
        EnterPage(s, opt, false);
}

// Closing: the page puts the tab back instead of storing the game's settings.
void OptStore(State& s) {
    {
        std::lock_guard lk(g_mutex);
        if (!g_active || s.x[0] != g_page) {
            RemoveEntryRow();
            return RunOriginal(s, g_hooks[kHStore]);
        }
        PutTabBack();
        RemoveEntryRow();
    }
    LOG_INFO("Menu", "Graphics page closed");
    GuestReturn(s, 0);
}

bool OnPage(u64 opt) { return g_active.load(std::memory_order_acquire) && opt == g_page; }

void ChoiceCount(State& s) {
    if (!OnPage(s.x[0]) || R32(s.x[0] + kObjTab) != kTab)
        return RunOriginal(s, g_hooks[kHCount]);
    const u32 row = u32(s.x[1]);
    GuestReturn(s, row < Cur().rows.size() ? Cur().rows[row].choices.size() : 0);
}

void Blocked(State& s, Hook h) {
    if (OnPage(s.x[0]))
        return GuestReturn(s, 0);
    RunOriginal(s, g_hooks[h]);
}
// L / R on the page: the other page, the way the game switches tabs (header animations, rows laid out again, the
// tab sound).
void SwitchPage(State& s, Hook h) {
    const u64 opt = s.x[0];
    if (!OnPage(opt))
        return RunOriginal(s, g_hooks[h]);
    if (R32(opt + kObjState) != kStateIdle)
        return GuestReturn(s, 0);
    int from = 0, to = 0;
    {
        std::lock_guard lk(g_mutex);
        Apply();
        from = g_cur;
        to = g_cur = 1 - g_cur;
        Show(Cur());
    }
    GuestCall(s, g_main + kPlayAnim, {opt + kObjAnims, 0, 0});
    GuestCall(s, g_main + kPlayAnim, {opt + kObjAnims, g_mem->Read<u8>(g_main + kTabAnims + kPageTabs[to]), 0});
    GuestCall(s, g_main + kPlayAnim, {opt + kObjAnims, g_mem->Read<u8>(g_main + kTabOffAnims + kPageTabs[from]), 0});
    GuestCall(s, g_main + kFillRows, {opt});
    if (const u64 sound = R64(g_main + kSoundGot))
        GuestCall(s, g_main + kPlaySound, {sound + 0x168, kSoundTab});
    GuestCall(s, g_main + kRowsDone, {opt});
    HideOthers(s, opt);
    LOG_INFO("Menu", "{} page shown", to == 0 ? "GRAPHICS" : "PC SETTINGS");
    GuestReturn(s, 0);
}
void TabPrev(State& s) { SwitchPage(s, kHTabPrev); }
void TabNext(State& s) { SwitchPage(s, kHTabNext); }
u32 CursorRow(State& s, u64 opt) {
    const u64 cursor = R64(opt + 0xb0);
    return cursor ? u32(GuestCall(s, R64(R64(cursor) + 0x58), {cursor})) : ~0u;
}
bool OnEntryRow(State& s, u64 opt) {
    return !OnPage(opt) && R32(opt + kObjTab) == kTab && CursorRow(s, opt) == kEntryRow;
}

// A on the GRAPHICS row: our pages, in place. A on our pages: the row's next value, wrapping round (like right).
void Decide(State& s) {
    const u64 opt = s.x[0];
    if (!OnPage(opt)) {
        if (!OnEntryRow(s, opt) || R32(opt + kObjState) != kStateIdle)
            return RunOriginal(s, g_hooks[kHDecide]);
        EnterPage(s, opt, true);
        return GuestReturn(s, 0);
    }
    const u32 row = CursorRow(s, opt);
    if (row < Cur().rows.size()) {
        u8 v[16];
        g_mem->ReadBlock(Cur().values, v, sizeof(v));
        v[row] = u8((v[row] + 1) % Cur().rows[row].choices.size());
        g_mem->WriteBlock(Cur().values, v, sizeof(v));
        GuestCall(s, g_main + kShowValue, {opt, row});
    }
    GuestReturn(s, 0);
}

// B on our pages opened from the GRAPHICS row: back to CONTROLS (the cursor on its first row, as on a tab switch).
void Back(State& s) {
    const u64 opt = s.x[0];
    if (!OnPage(opt) || !g_from_options)
        return RunOriginal(s, g_hooks[kHBack]);
    if (R32(opt + kObjState) != kStateIdle)
        return GuestReturn(s, 0);
    const int cur = g_cur;
    {
        std::lock_guard lk(g_mutex);
        PutTabBack();
    }
    if (const u64 sound = R64(g_main + kSoundGot))
        GuestCall(s, g_main + kPlaySound, {sound + 0x168, kSoundBack});
    const u64 tabs = R64(opt + kObjTabLayout);
    for (u32 t = 0; t < 5; t++) // the headers HideOthers hid
        if (t != kPageTabs[0] && t != kPageTabs[1])
            for (u32 p = 0; p < kPanesPerTab; p++)
                if (const u64 pane = tabs ? GuestCall(s, g_main + kPane, {tabs, kTabPanes + kPanesPerTab * t + p}) : 0)
                    GuestCall(s, g_main + kSetVisible, {pane, 1});
    if (cur == 1) { // PC SETTINGS lit tab 3's header
        GuestCall(s, g_main + kPlayAnim, {opt + kObjAnims, g_mem->Read<u8>(g_main + kTabOffAnims + kPageTabs[1]), 0});
        GuestCall(s, g_main + kPlayAnim, {opt + kObjAnims, g_mem->Read<u8>(g_main + kTabAnims + kTab), 0});
    }
    GuestCall(s, g_main + kTabNames, {opt}); // CONTROLS and SOUND again
    g_mem->Write<u32>(opt + kObjTab, kTab);
    GuestCall(s, g_main + kFillRows, {opt});
    GuestCall(s, g_main + kRowsDone, {opt});
    LOG_INFO("Menu", "Graphics page closed, back to OPTIONS");
    GuestReturn(s, 0);
}

// While the page is open its value bytes are watched (an arrow row commits its new value some frames after the
// key, at the end of its animation): a change applies at once.
void Watch() {
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        std::lock_guard lk(g_mutex);
        if (g_active)
            Apply();
    }
}

// Y (DEFAULT) on the page: the row goes back to the launcher's default.
void Default(State& s) {
    const u64 opt = s.x[0];
    if (!OnPage(opt)) {
        if (OnEntryRow(s, opt)) // nothing to reset on the GRAPHICS row
            return GuestReturn(s, 0);
        return RunOriginal(s, g_hooks[kHDefault]);
    }
    const u32 row = CursorRow(s, opt);
    if (row < Cur().rows.size()) {
        const core::Config now = PageConfig();
        core::Config defaults;
        defaults.fps_limit = 0; // the launcher's presets: no cap
        u8 v[16], d[16]{};
        g_mem->ReadBlock(Cur().values, v, sizeof(v));
        Cur().to(defaults, d);
        v[row] = d[row];
        ToPage(Cur(), now); // the custom value texts
        g_mem->WriteBlock(Cur().values, v, sizeof(v));
        GuestCall(s, g_main + kShowValue, {opt, row});
    }
    GuestReturn(s, 0);
}

void GetString(State& s) {
    const u32 h = u32(s.x[1]);
    if ((h & 0xFF000000) == kOurs) {
        const u32 i = h - kOurs;
        if (i < g_strings.size())
            return GuestReturn(s, g_strings[i]);
    } else if ((h == kStrControls || h == kStrSound) &&
               (g_active.load(std::memory_order_relaxed) || g_open_next.load(std::memory_order_relaxed))) {
        const u32 title = g_pages[h == kStrControls ? 0 : 1].title; // the pages' headers
        if ((title & 0xFF000000) == kOurs)
            return GuestReturn(s, g_strings[title - kOurs]);
        s.x[1] = title;
    }
    RunOriginal(s, g_hooks[kHString]);
}

} // namespace

void OpenNext() { g_open_next = true; }

void SetRenderScale(u32 scale3) { g_scale3 = scale3; }

u32 LabelHash() { return kStrGraphics; }

void Install(core::Memory& mem, kernel::Kernel& kernel, u64 main_base) {
    // the build these offsets come from: the first words of the functions, the row counts and tab 2's row types
    static constexpr std::pair<u64, u32> kExpect[] = {
        {kOptInit, 0xf81d0ff5},   {kOptStore, 0xa9be4ff4},  {kChoiceCount, 0xb9419808}, {kTabPrev, 0xa9bd57f6},
        {kTabNext, 0xa9bd57f6},   {kValuePrev, 0xa9bc5ff8}, {kValueNext, 0xa9bc5ff8},   {kDecide, 0xf81c0ff7},
        {kDefault, 0xa9bd57f6},   {kShowValue, 0xfc1b0fe8},
        {kFillRows, 0xfc170fec},  {kRowsDone, 0xfc1c0fea},  {kGetString, 0x2a0103e8},  {kRowCounts + 4, 0x40003},
        {kRowTypes, 1},           {kRowTypes + 4 * kEntryRow, 0}, {kBack, 0xf81e0ff3}, {kTabNames, 0xfc1a0fe8},
    };
    for (auto [off, word] : kExpect)
        if (!mem.IsValid(main_base + off, 4) || mem.Read<u32>(main_base + off) != word) {
            LOG_WARN("Menu", "unknown game build (main+{:#x}): graphics page off", off);
            return;
        }
    g_mem = &mem;
    g_main = main_base;
    BuildPages();
    g_entry_info = Text(u"Display mode, resolution, frame rate and anti-aliasing. R: PC SETTINGS.");
    const std::pair<u64, void (*)(State&)> hooks[kHookCount] = {
        {kOptInit, OptInit},     {kOptStore, OptStore},   {kChoiceCount, ChoiceCount}, {kTabPrev, TabPrev},
        {kTabNext, TabNext},     {kDecide, Decide},       {kDefault, Default},         {kGetString, GetString},
        {kBack, Back},
    };
    for (int i = 0; i < kHookCount; i++) {
        g_hooks[i].pc = main_base + hooks[i].first;
        g_hooks[i].host = hooks[i].second;
        g_hooks[i].direct = i == kHStore || i == kHCount || i == kHString; // also called with BL
        kernel.Cpus().Override(g_hooks[i]);
    }
    std::thread(Watch).detach();
    LOG_INFO("Menu", "graphics pages installed ({} + {} rows)", g_pages[0].rows.size(), g_pages[1].rows.size());
}

} // namespace hle::menu::graphics
