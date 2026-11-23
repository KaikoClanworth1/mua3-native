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

#include "core/hle/keybinds.h"
#include "core/hle/menu_guest.h"
#include "core/hle/mods.h"
#include "core/kernel/kernel.h"
#include "core/memory.h"
#include "core/hle/prompts.h"
#include "core/upscalers.h"

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
constexpr u64 kShowValue = 0x7c8970;                  // (this, slot): draws a row's value (after every change)
constexpr u64 kGreyRow = 0x7caf30;                    // (this, slot, string hash, 0): a row greyed out (tab 1's way)
constexpr u64 kUngreyRow = 0x7cb230;                  // (this, slot, 0): back to normal
constexpr u64 kObjRowOff = 0x1ac;                     // u32 per visible row: 1 = greyed out (left / right do nothing)
constexpr u64 kFillRows = 0x7c5900;                   // (this): lays out the current tab's rows
constexpr u64 kCursorShow = 0x7cc0d0;                 // (this): row highlights redrawn for the row at this+0x1fc
constexpr u64 kObjCursorRow = 0x1fc;                  // u32: the row the cursor is on (SetCursor: cursor vfunc +0x60)
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

enum RowType : u32 { kButton = 0, kTwoChoices = 1, kArrows = 2 };

constexpr std::pair<int, int> kResolutions[] = {{1280, 720},  {1600, 900},  {1920, 1080}, {2560, 1440}, {3200, 1800},
                                                {3840, 2160}, {2560, 1080}, {3440, 1440}, {5120, 2880}};
constexpr int kRates[] = {30, 60, 72, 90, 120, 144, 165, 240}; // as the launcher offers them
constexpr int kLimits[] = {0, 30, 60, 72, 90, 120, 144, 165, 240};

constexpr u32 kMaxRows = 8; // tab 2's row-type table has room for 8 (the next table is tab 0's)
// A page shows at most 8 rows (FillRows lays out 8 and does not scroll), so FPS LIMIT and SHOW FPS live on PC SETTINGS.
enum GraphicsRows {
    kRowMode, kRowResolution, kRowScale, kRowUpscaler, kRowSharpness, kRowAa, kRowRate, kRowVsync, kGraphicsRows
};
enum PcRows { kRowLimit, kRowShowFps, kRowVolume, kRowMute, kRowAudio, kRowControls, kRowMods, kPcRows };
// CONTROLS (opened from PC SETTINGS' CONTROLS row); room for more rows (stick deadzones, #17)
enum ControlsRows { kRowController, kRowButtons, kRowCamera, kRowLeftDead, kRowRightDead, kRowKeys, kControlsRows };
constexpr int kDeadStep = 2, kDeadMax = 50; // stick deadzones: 0-50% in steps of 2 (another ini value: its own text)
enum Aa { kAaOff, kAaFxaa, kAaSsaa2 }; // anti_aliasing: 2-4 = SSAA 2X-4X
// UPSCALER row: OFF, then 4 qualities (upscale_quality 0-3) of each upscaler= value shown, in this order. FSR 3.1 and
// DLSS are shown where they run, or when mua3.ini already asks for them; a value the row cannot show is left as it is.
constexpr int kUpscalerKinds[] = {core::kUpscalerFsr1, core::kUpscalerFsr3, core::kUpscalerDlss};
constexpr u32 kQualities = 4;
std::vector<int> g_kinds{core::kUpscalerFsr1}; // the upscalers the row shows (ShowUpscalers, when the page opens)
// upscaler= of a row value (0 = off)
int UpscalerOf(u8 v) { return v >= 1 && v <= kQualities * g_kinds.size() ? g_kinds[(v - 1) / kQualities] : 0; }
// FSR SHARPNESS applies to the FSR upscalers
bool Sharpened(int upscaler) { return upscaler == core::kUpscalerFsr1 || upscaler == core::kUpscalerFsr3; }
static_assert(kGraphicsRows <= kMaxRows && kPcRows <= kMaxRows && kControlsRows <= kMaxRows);
constexpr int kCameraSpeeds[] = {10, 20, 30, 40, 50, 60, 70, 80, 90, 100, 110, 120, 130, 140, 150, 160, 170, 180, 190, 200};
const char16_t* const kControllerNames[] = {u"XBOX (XINPUT)", u"ANY CONTROLLER (SDL)"};
const char16_t* const kButtonLayoutNames[] = {u"XBOX LETTERS", u"SWITCH POSITIONS"}; // button_layout 0 / 1
const char16_t* const kAudioNames[] = {u"XAUDIO2", u"SDL", u"OFF"};

core::Memory* g_mem = nullptr;
u64 g_main = 0;

// guest memory: strings, then the row table, choice arrays and value bytes
u64 g_block = 0, g_alloc = 0;
constexpr u64 kBlockSize = 0x10000; // the texts of all pages, with room
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
Page g_pages[5];
constexpr int kPageMods = 2;     // MODS, opened from PC SETTINGS' MODS row
constexpr int kPageControls = 3; // CONTROLS, opened from PC SETTINGS' CONTROLS row
constexpr int kPageKeys = 4;     // KEY BINDINGS, opened from CONTROLS' KEY BINDINGS row
int g_cur = 0; // the page shown
bool SubPage() { return g_cur >= kPageMods; } // a page opened from a PC SETTINGS row: one header, B goes back
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
    kHInit, kHStore, kHCount, kHTabPrev, kHTabNext, kHDecide, kHDefault, kHString, kHBack, kHValuePrev, kHValueNext, kHookCount
};
cpu::FunctionOverride g_hooks[kHookCount];
bool OnPage(u64 opt);
void UpdateGrey(State& s, u64 opt, int upscaler = -1, bool force = false);
void ModsPage(State& s, u64 opt, int dir);
void KeysPage(State& s, u64 opt, int dir);
void Exclude(State& s, u64 opt, u32 row, u8 value);

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

u32 g_custom_dead[2]{}; // LEFT / RIGHT STICK DEADZONE: a value off the steps
u32 g_custom_res = 0, g_custom_limit = 0, g_custom_volume = 0, g_custom_camera = 0,
    g_custom_sharpness = 0, g_sharpness_off = 0; // rewritable value texts; the greyed FSR SHARPNESS text
// the restart rows' info lines (they name the value running now)
u32 g_scale_info = 0, g_rate_info = 0, g_controller_info = 0, g_audio_info = 0;
const std::u16string kScaleInfo = u"The resolution the game renders at. AUTO matches your display.";
const std::u16string kRateInfo = u"The game's frame rate. 30 is the original, 60 is fully supported.";
const std::u16string kControllerInfo = u"XINPUT: Xbox controllers. SDL: PlayStation, Switch Pro and other controllers.";
const std::u16string kAudioInfo = u"How the game's sound is played. XAUDIO2 suits most PCs; OFF plays no sound.";
const std::u16string kScaleNames[] = {u"AUTO", u"1X (1080P)", u"2X (1440P / 4K)", u"3X", u"4X"};
constexpr size_t kInfoRoom = 200;
std::atomic<u32> g_scale3{0}; // the running render scale in thirds (0 = not told)

// MODS: MODS ENABLED, then up to 7 mods (L / R page through more). Y picks a mod up, left / right move it in the load
// order, A or Y puts it down.
constexpr u32 kModRows = kMaxRows - 1;
constexpr size_t kLabelRoom = 48;
std::vector<mods::Mod> g_mods; // the list shown, in load order (our copy: edited, then saved to order.txt)
size_t g_mod_first = 0;        // the first mod shown
int g_moving = -1;             // the mod picked up with Y
bool g_launch_mods_enabled = true;
u32 g_mods_title = 0, g_master_info = 0, g_no_mods = 0, g_mod_label[kModRows]{}, g_mod_info[kModRows]{};
std::vector<RowInfo> g_mod_rows; // all 8; the page shows the first ones

// KEY BINDINGS: RESET ALL, then 7 actions a page (L / R: the others). A on an action takes the next key or mouse
// button pressed (hle::keybinds' capture), Y puts the action's default back.
constexpr u32 kKeyRows = kMaxRows - 1;
u32 g_key_first = 0;                 // the first action shown
u32 g_keys_title = 0, g_keys_info = 0, g_key_label[kKeyRows]{}, g_key_info[kKeyRows]{}, g_key_value[kKeyRows]{};
std::vector<RowInfo> g_key_rows;     // all 8
int g_key_note_action = -1;          // the action whose info line tells of a swap
std::u16string g_key_note;

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

// The UPSCALER row's choices: OFF, then those of the upscalers that run here (or that mua3.ini asks for), packed.
void ShowUpscalers() {
    const int want = core::Sys().CfgSnapshot().upscaler;
    g_kinds.clear();
    const RowInfo& r = g_pages[0].rows[kRowUpscaler];
    for (u32 k = 0; k < std::size(kUpscalerKinds); k++) {
        const int kind = kUpscalerKinds[k];
        if (kind != core::kUpscalerFsr1 && !core::UpscalerAvailable(kind) && want != kind)
            continue;
        for (u32 q = 0; q < kQualities; q++)
            W32(r.guest_choices + 4 * (1 + kQualities * g_kinds.size() + q), r.choices[1 + kQualities * k + q]);
        g_kinds.push_back(kind);
    }
}
void FromGraphics(core::Config& c, bool running, const u8* v);
void ToPc(const core::Config& c, u8* v);
void FromPc(core::Config& c, bool running, const u8* v);
void ToMods(const core::Config& c, u8* v);
void FromMods(core::Config& c, bool running, const u8* v);
void ToControls(const core::Config& c, u8* v);
void FromControls(core::Config& c, bool running, const u8* v);
void ToKeys(const core::Config&, u8*) {} // (button rows only: the bindings live in hle::keybinds)
void FromKeys(core::Config&, bool, const u8*) {}

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
    std::vector<u32> ups{kStrOff};
    for (const char16_t* name : {u"FSR 1", u"FSR 3.1", u"DLSS"})
        for (const char16_t* q : {u" ULTRA QUALITY", u" QUALITY", u" BALANCED", u" PERFORMANCE"})
            ups.push_back(Text(std::u16string(name) + q));
    add(Text(u"UPSCALER"),
        Text(u"Renders below the output size and upscales (faster).\r\n"
             u"ULTRA QUALITY renders the most pixels, PERFORMANCE the fewest. No SSAA."),
        kArrows, ups);
    std::vector<u32> sharp;
    for (int v = 0; v <= 100; v += 10)
        sharp.push_back(Text(U16(v == 80 ? "80% (DEFAULT)" : fmt::format("{}%", v))));
    g_custom_sharpness = Text(u"", 8);
    g_sharpness_off = Text(u"Pick an FSR upscaler first.");
    sharp.push_back(g_custom_sharpness);
    add(Text(u"FSR SHARPNESS"), Text(u"How much FSR sharpens the upscaled picture. Used only while FSR is on."),
        kArrows, sharp);
    add(Text(u"ANTI-ALIASING"),
        Text(u"Smooths jagged edges. FXAA costs nothing; SSAA renders more pixels (high VRAM).\r\n"
             u"SSAA multiplies the render scale (4X in total at most). With FSR on: OFF or FXAA."),
        kArrows, {kStrOff, Text(u"FXAA"), Text(u"SSAA 2X"), Text(u"SSAA 3X"), Text(u"SSAA 4X")});
    std::vector<u32> rates;
    for (int r : kRates)
        rates.push_back(Text(U16(r == 30 ? "30 (ORIGINAL)" : r > 60 ? fmt::format("{} (EXPERIMENTAL)", r)
                                                                   : std::to_string(r))));
    g_rate_info = Text(kRateInfo, kInfoRoom);
    add(Text(u"FRAME RATE"), g_rate_info, kArrows, rates);
    add(Text(u"VSYNC"), Text(u"Synchronizes frames with the display to prevent tearing."), kTwoChoices,
        {kStrOn, kStrOff});
    Finish(*page);
    // PC SETTINGS
    page = &g_pages[1];
    page->title = Text(u"PC SETTINGS");
    page->to = ToPc;
    page->from = FromPc;
    std::vector<u32> limits;
    for (int l : kLimits)
        limits.push_back(l == 0 ? kStrOff : Text(U16(std::to_string(l))));
    g_custom_limit = Text(u"", 8);
    limits.push_back(g_custom_limit);
    add(Text(u"FPS LIMIT"), Text(u"Caps how fast frames are shown. OFF = no cap."), kArrows, limits);
    add(Text(u"SHOW FPS"), Text(u"Shows the frame rate in the corner of the screen."), kTwoChoices,
        {kStrOn, kStrOff});
    std::vector<u32> volumes;
    for (int v = 0; v <= 100; v += 10)
        volumes.push_back(Text(U16(fmt::format("{}%", v))));
    g_custom_volume = Text(u"", 8);
    volumes.push_back(g_custom_volume);
    add(Text(u"MASTER VOLUME"), Text(u"The volume of all the game's sound, on top of the SOUND tab's own volumes."),
        kArrows, volumes);
    add(Text(u"MUTE"), Text(u"Silences the game. The volume is kept for when you switch it back off."), kTwoChoices,
        {kStrOn, kStrOff});
    g_audio_info = Text(kAudioInfo, kInfoRoom);
    add(Text(u"AUDIO OUTPUT"), g_audio_info, kArrows,
        {Text(kAudioNames[0]), Text(kAudioNames[1]), Text(kAudioNames[2])});
    const u32 open = Text(u"OPEN");
    add(Text(u"CONTROLS"), Text(u"Controller type, button layout, camera speed, stick deadzones and key bindings."),
        kButton, {open});
    add(Text(u"MODS"), Text(u"Turn mods on or off and set their load order. Mods apply after a restart."), kButton,
        {open});
    Finish(*page);
    // CONTROLS
    page = &g_pages[kPageControls];
    page->title = Text(u"CONTROLS");
    page->to = ToControls;
    page->from = FromControls;
    g_controller_info = Text(kControllerInfo, kInfoRoom);
    add(Text(u"CONTROLLER TYPE"), g_controller_info, kArrows, {Text(kControllerNames[0]), Text(kControllerNames[1])});
    add(Text(u"BUTTONS"),
        Text(u"XBOX LETTERS: press the button the game shows.\r\n"
             u"SWITCH POSITIONS: the Switch's layout (A right, B bottom)."),
        kArrows, {Text(kButtonLayoutNames[0]), Text(kButtonLayoutNames[1])});
    std::vector<u32> speeds;
    for (int v : kCameraSpeeds)
        speeds.push_back(Text(U16(v == 100 ? "100% (ORIGINAL)" : fmt::format("{}%", v))));
    g_custom_camera = Text(u"", 8);
    speeds.push_back(g_custom_camera);
    add(Text(u"CAMERA SPEED"), Text(u"How fast the right stick turns the camera. 100% is the original speed.\r\n"
             u"Multiplies the CAMERA tab's CONTROL SENSITIVITY."), kArrows,
        speeds);
    const u32 dead_info = Text(u"How far the stick moves before it counts. Lower = more responsive;\r\n"
                               u"raise it if the camera or hero drifts.");
    for (int k = 0; k < 2; k++) {
        std::vector<u32> dead;
        for (int v = 0; v <= kDeadMax; v += kDeadStep)
            dead.push_back(Text(U16(v == 10 ? "10% (DEFAULT)" : fmt::format("{}%", v))));
        g_custom_dead[k] = Text(u"", 8);
        dead.push_back(g_custom_dead[k]);
        add(Text(k == 0 ? u"LEFT STICK DEADZONE" : u"RIGHT STICK DEADZONE"), dead_info, kArrows, dead);
    }
    add(Text(u"KEY BINDINGS"), Text(u"Which keys and mouse buttons play the game. Saved and used at once."), kButton,
        {open});
    Finish(*page);
    // KEY BINDINGS
    page = &g_pages[kPageKeys];
    g_keys_title = Text(u"KEY BINDINGS", 24);
    page->title = g_keys_title;
    page->to = ToKeys;
    page->from = FromKeys;
    g_keys_info = Text(u"", kInfoRoom);
    add(Text(u"RESET ALL"), g_keys_info, kButton, {Text(u"RESET")});
    for (u32 k = 0; k < kKeyRows; k++) {
        g_key_label[k] = Text(u"", kLabelRoom);
        g_key_info[k] = Text(u"", kInfoRoom);
        g_key_value[k] = Text(u"", kLabelRoom);
        // (an arrow row with one choice: the game redraws an arrow row's value text in place, a button row's only
        // at layout)
        add(g_key_label[k], g_key_info[k], kArrows, {g_key_value[k]});
    }
    Finish(*page);
    g_key_rows = page->rows;
    // MODS
    page = &g_pages[kPageMods];
    g_mods_title = Text(u"MODS", 16);
    page->title = g_mods_title;
    page->to = ToMods;
    page->from = FromMods;
    g_master_info = Text(u"", kInfoRoom);
    add(Text(u"MODS ENABLED"), g_master_info, kTwoChoices, {kStrOn, kStrOff});
    for (u32 k = 0; k < kModRows; k++) {
        g_mod_label[k] = Text(u"", kLabelRoom);
        g_mod_info[k] = Text(u"", kInfoRoom);
        add(g_mod_label[k], g_mod_info[k], kTwoChoices, {kStrOn, kStrOff});
    }
    g_no_mods = Text(u"Put mods in the mods folder next to mua3.exe.");
    Finish(*page);
    g_mod_rows = page->rows;
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
    v[kRowVsync] = c.vsync ? 0 : 1;
    v[kRowUpscaler] = 0;
    for (u32 k = 0; k < g_kinds.size(); k++)
        if (c.upscaler == g_kinds[k])
            v[kRowUpscaler] = u8(1 + kQualities * k + std::clamp(c.upscale_quality, 0, 3));
    PutString(g_custom_sharpness, U16(fmt::format("{}%", c.sharpness)), 8);
    v[kRowSharpness] = u8(c.sharpness % 10 == 0 ? std::clamp(c.sharpness, 0, 100) / 10 : 11);
    // the renderer runs no SSAA while FSR upscales: the row shows what runs
    const int aa = std::clamp(c.anti_aliasing, 0, 4);
    v[kRowAa] = u8(c.upscaler != 0 && aa >= kAaSsaa2 ? kAaFxaa : aa);
}
void ToPc(const core::Config& c, u8* v) {
    PutString(g_custom_limit, U16(std::to_string(c.fps_limit)), 8);
    v[kRowLimit] = u8(IndexOf(kLimits, c.fps_limit, int(std::size(kLimits))));
    v[kRowShowFps] = c.show_fps ? 0 : 1;
    PutString(g_custom_volume, U16(fmt::format("{}%", c.volume)), 8);
    v[kRowVolume] = u8(c.volume % 10 == 0 ? std::clamp(c.volume, 0, 100) / 10 : 11);
    v[kRowMute] = c.mute ? 0 : 1;
    v[kRowAudio] = u8(std::clamp(int(c.audio), 0, 2));
}
void ToControls(const core::Config& c, u8* v) {
    v[kRowController] = u8(std::clamp(int(c.input), 0, 1));
    v[kRowButtons] = u8(std::clamp(c.button_layout, 0, 1));
    PutString(g_custom_camera, U16(fmt::format("{}%", c.camera_speed)), 8);
    v[kRowCamera] = u8(IndexOf(kCameraSpeeds, c.camera_speed, int(std::size(kCameraSpeeds))));
    const int dead[2] = {c.left_deadzone, c.right_deadzone};
    for (int k = 0; k < 2; k++) {
        PutString(g_custom_dead[k], U16(fmt::format("{}%", dead[k])), 8);
        v[kRowLeftDead + k] = u8(dead[k] % kDeadStep == 0 && dead[k] >= 0 && dead[k] <= kDeadMax ? dead[k] / kDeadStep
                                                                                                  : kDeadMax / kDeadStep + 1);
    }
}

// The value bytes as settings: `running` = only what can change while the game runs.
void FromGraphics(core::Config& c, bool running, const u8* v) {
    c.fullscreen = v[kRowMode] == 1;
    if (v[kRowResolution] < std::size(kResolutions))
        std::tie(c.width, c.height) = kResolutions[v[kRowResolution]];
    c.vsync = v[kRowVsync] == 0;
    c.anti_aliasing = std::min<int>(v[kRowAa], 4);
    if (v[kRowUpscaler] == 0) // live, like the anti-aliasing
        c.upscaler = 0;
    else if (const int up = UpscalerOf(v[kRowUpscaler])) {
        c.upscaler = up;
        c.upscale_quality = int((v[kRowUpscaler] - 1) % kQualities);
    }
    if (v[kRowSharpness] <= 10)
        c.sharpness = v[kRowSharpness] * 10;
    if (running)
        return;
    c.resolution_scale = std::min<int>(v[kRowScale], 4);
    if (v[kRowRate] < std::size(kRates))
        c.fps_target = kRates[v[kRowRate]];
}
void FromPc(core::Config& c, bool running, const u8* v) {
    if (v[kRowLimit] < std::size(kLimits))
        c.fps_limit = kLimits[v[kRowLimit]];
    c.show_fps = v[kRowShowFps] == 0;
    if (v[kRowVolume] <= 10)
        c.volume = v[kRowVolume] * 10;
    c.mute = v[kRowMute] == 0;
    if (running)
        return;
    c.audio = core::AudioBackend(std::min<int>(v[kRowAudio], 2));
}
void FromControls(core::Config& c, bool running, const u8* v) {
    c.button_layout = std::min<int>(v[kRowButtons], 1); // live
    if (v[kRowCamera] < std::size(kCameraSpeeds))
        c.camera_speed = kCameraSpeeds[v[kRowCamera]];
    if (v[kRowLeftDead] <= kDeadMax / kDeadStep) // live, like the camera speed
        c.left_deadzone = v[kRowLeftDead] * kDeadStep;
    if (v[kRowRightDead] <= kDeadMax / kDeadStep)
        c.right_deadzone = v[kRowRightDead] * kDeadStep;
    if (running)
        return;
    c.input = core::InputBackend(std::min<int>(v[kRowController], 1));
}

// MODS: the master switch is mua3.ini [mods] enabled; the mods' own switches and order go to mods\order.txt.
size_t ShownMods() { return std::min<size_t>(kModRows, g_mods.size() - std::min(g_mod_first, g_mods.size())); }
void ToMods(const core::Config& c, u8* v) {
    v[0] = c.mods_enabled ? 0 : 1;
    v[1] = 1; // the empty list's NO MODS FOUND row
    for (size_t k = 0; k < ShownMods(); k++)
        v[1 + k] = g_mods[g_mod_first + k].enabled ? 0 : 1;
}
void FromMods(core::Config& c, bool, const u8* v) { c.mods_enabled = v[0] == 0; }

std::u16string FromUtf8(const std::string& s) {
    std::u16string out;
    for (size_t i = 0; i < s.size();) {
        const u8 b = u8(s[i]);
        const int n = b < 0x80 ? 1 : (b >> 5) == 6 ? 2 : (b >> 4) == 14 ? 3 : (b >> 3) == 30 ? 4 : 1;
        u32 cp = n == 1 ? b : b & (0x7f >> n);
        for (int k = 1; k < n && i + k < s.size(); k++)
            cp = (cp << 6) | (u8(s[i + k]) & 0x3f);
        i += n;
        if (cp >= 0x10000) { // a surrogate pair
            cp -= 0x10000;
            out += char16_t(0xd800 + (cp >> 10));
            out += char16_t(0xdc00 + (cp & 0x3ff));
        } else
            out += char16_t(cp);
    }
    return out;
}
std::u16string Upper(std::u16string t) {
    for (char16_t& ch : t)
        if (ch >= u'a' && ch <= u'z')
            ch = char16_t(ch - u'a' + u'A');
    return t;
}

// The MODS page's rows and texts for the mods shown (g_mutex held).
void BuildModsRows() {
    Page& page = g_pages[kPageMods];
    const size_t n = g_mods.size(), shown = ShownMods();
    const bool changed = mods::ChangedSinceLaunch() || core::Sys().CfgSnapshot().mods_enabled != g_launch_mods_enabled;
    const std::u16string restart = changed ? u"\r\nApplies after a restart." : u"";
    const size_t pages = (n + kModRows - 1) / kModRows;
    PutString(g_mods_title, pages > 1 ? U16(fmt::format("MODS {} / {}", g_mod_first / kModRows + 1, pages)) : u"MODS",
              16);
    if (n == 0) {
        PutString(g_master_info, u"No mods found. Put mods in the mods folder next to mua3.exe." + restart, kInfoRoom);
        PutString(g_mod_label[0], u"NO MODS FOUND", kLabelRoom);
        PutString(g_mod_info[0], u"Put mods in the mods folder next to mua3.exe, one folder per mod." + restart,
                  kInfoRoom);
        page.rows.assign(g_mod_rows.begin(), g_mod_rows.begin() + 2);
        return;
    }
    PutString(g_master_info,
              u"Turns all mods on or off. Mods lower in the list win when two change the same file." +
                  (pages > 1 ? U16(fmt::format("\r\nL / R: more mods ({} in all).", n)) : std::u16string()) + restart,
              kInfoRoom);
    const std::vector<mods::Mod> loaded = mods::Loaded();
    for (size_t k = 0; k < shown; k++) {
        const size_t i = g_mod_first + k;
        const mods::Mod& m = g_mods[i];
        std::u16string name = Upper(FromUtf8(m.name));
        if (name.size() > kLabelRoom - 8)
            name = name.substr(0, kLabelRoom - 11) + u"...";
        PutString(g_mod_label[k], int(i) == g_moving ? u"> " + name + u" <" : name, kLabelRoom);
        std::u16string info;
        if (int(i) == g_moving) {
            info = u"Moving: LEFT / RIGHT moves it up / down the load order. A or Y puts it down.";
        } else {
            if (m.romfs.empty())
                info = u"No game files found in this mod.";
            else {
                auto it = std::find_if(loaded.begin(), loaded.end(), [&](const mods::Mod& l) { return l.name == m.name; });
                info = it != loaded.end() && it->loaded
                           ? U16(fmt::format("Replaces {} game files, adds {}.", it->overrides, it->addons))
                           : std::u16string(u"Not loaded this launch.");
            }
            if (!m.unsupported.empty()) {
                std::string parts;
                for (const std::string& u : m.unsupported) // the part's name only: the info line has two lines
                    parts += (parts.empty() ? "" : ", ") + u.substr(0, u.find(" ("));
                info += U16(" Not supported: " + parts + ".");
            }
            info += U16(fmt::format("\r\nLoad order {} of {}. Y: move it.", i + 1, n));
        }
        PutString(g_mod_info[k], info + restart, kInfoRoom);
    }
    page.rows.assign(g_mod_rows.begin(), g_mod_rows.begin() + 1 + shown);
}

// The mods' ON / OFF rows changed: into our list and order.txt (g_mutex held).
void SyncMods(const u8* v) {
    bool changed = false;
    for (size_t k = 0; k < ShownMods(); k++) {
        mods::Mod& m = g_mods[g_mod_first + k];
        if (m.enabled != (v[1 + k] == 0)) {
            m.enabled = v[1 + k] == 0;
            changed = true;
        }
    }
    if (changed && !mods::SaveOrder(g_mods))
        LOG_WARN("Menu", "could not save mods\\order.txt");
}

// The KEY BINDINGS page's rows and texts for the actions shown (g_mutex held).
void BuildKeyRows() {
    namespace kb = keybinds;
    const u32 pages = (kb::kActions + kKeyRows - 1) / kKeyRows;
    const u32 shown = std::min<u32>(kKeyRows, kb::kActions - g_key_first);
    PutString(g_keys_title, U16(fmt::format("KEY BINDINGS {} / {}", g_key_first / kKeyRows + 1, pages)), 24);
    PutString(g_keys_info,
              U16(fmt::format("Puts every key and mouse button back to the defaults.\r\nL / R: more ({} in all).",
                              int(kb::kActions))),
              kInfoRoom);
    const int capturing = kb::Capturing();
    for (u32 k = 0; k < shown; k++) {
        const auto a = kb::Action(g_key_first + k);
        PutString(g_key_label[k], kb::Label(a), kLabelRoom);
        PutString(g_key_value[k], capturing == int(a) ? u"PRESS A KEY..." : kb::Text(kb::Get(a)), kLabelRoom);
        std::u16string info;
        if (capturing == int(a))
            info = u"Press a key or mouse button for " + kb::Label(a) + u".\r\nESC: cancel. BACKSPACE: no key.";
        else if (g_key_note_action == int(a))
            info = g_key_note;
        else
            info = u"A, then a key or mouse button: change it. Y: default (" + kb::Text(kb::Default(a)) + u").\r\n" +
                   (kb::Note(a).empty() ? std::u16string(u"ESC cancels a change, BACKSPACE leaves no key.") : kb::Note(a));
        PutString(g_key_info[k], info, kInfoRoom);
    }
    g_pages[kPageKeys].rows.assign(g_key_rows.begin(), g_key_rows.begin() + 1 + shown);
}

// The bindings changed: into mua3.ini and the keyboard prompts.
void SaveKeys() {
    core::Sys().UpdateConfig([](core::Config& c, bool) { keybinds::Store(c); });
    prompts::KeysChanged();
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
        u32 f = c.upscaler == 0 && c.anti_aliasing >= 2 && c.anti_aliasing <= 4 ? u32(c.anti_aliasing) : 1;
        while (f > 1 && s3 * f > 12)
            f--;
        size = c.upscaler != 0 ? std::u16string(u"\r\nAn upscaler is on: the UPSCALER sets the render size.")
                               : u"\r\nRenders at " + dims(s3) +
                                     (f > 1 ? u" (" + dims(s3 * f) + u" with SSAA " + U16(std::to_string(f)) + u"X)"
                                            : u"") +
                                     u" now.";
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
    if (g_cur == kPageMods)
        SyncMods(v);
    core::Sys().UpdateConfig([&page, &v](core::Config& c, bool running) { page.from(c, running, v); });
    WriteRestartInfo(); // the render size follows the anti-aliasing
    if (g_cur == kPageMods)
        BuildModsRows(); // "Applies after a restart."
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
        if (t != kPageTabs[0] && (t != kPageTabs[1] || SubPage()))
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
        g_moving = -1;
        ShowUpscalers();
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
    UpdateGrey(s, opt, -1, true);
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
    if (g_cur == 0 && row == kRowUpscaler) // the upscalers shown
        return GuestReturn(s, 1 + kQualities * g_kinds.size());
    GuestReturn(s, row < Cur().rows.size() ? Cur().rows[row].choices.size() : 0);
}

void Blocked(State& s, Hook h) {
    if (OnPage(s.x[0]))
        return GuestReturn(s, 0);
    RunOriginal(s, g_hooks[h]);
}
// FSR SHARPNESS is greyed out while the UPSCALER is OFF (the way the CAMERA tab greys a row; rows sit at their own
// slot, the page does not scroll).
void UpdateGrey(State& s, u64 opt, int upscaler, bool force) {
    if (!OnPage(opt))
        return;
    const bool fsr = g_cur == 0 &&
                     Sharpened(UpscalerOf(u8(upscaler >= 0 ? upscaler : g_mem->Read<u8>(Cur().values + kRowUpscaler))));
    for (u32 slot = 0; slot < kMaxRows; slot++) {
        u32 text = 0;
        if (g_cur == 0 && slot == kRowSharpness && !fsr)
            text = g_sharpness_off;
        else if (g_cur == kPageMods && g_mods.empty() && slot == 1)
            text = g_no_mods;
        bool grey = R32(opt + kObjRowOff + 4 * slot) != 0;
        if (grey && (!text || force)) {
            GuestCall(s, g_main + kUngreyRow, {opt, slot, 0});
            W32(opt + kObjRowOff + 4 * slot, 0);
            grey = false;
        }
        if (!grey && text) {
            GuestCall(s, g_main + kGreyRow, {opt, slot, text, 0});
            W32(opt + kObjRowOff + 4 * slot, 1);
        }
    }
}

// L / R on the page: the other page, the way the game switches tabs (header animations, rows laid out again, the
// tab sound).
void SwitchPage(State& s, Hook h) {
    const u64 opt = s.x[0];
    if (!OnPage(opt))
        return RunOriginal(s, g_hooks[h]);
    if (R32(opt + kObjState) != kStateIdle)
        return GuestReturn(s, 0);
    if (g_cur == kPageMods)
        return ModsPage(s, opt, h == kHTabPrev ? -1 : 1);
    if (g_cur == kPageKeys)
        return KeysPage(s, opt, h == kHTabPrev ? -1 : 1);
    if (SubPage()) // one page
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
    UpdateGrey(s, opt, -1, true);
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

void PlaySound(State& s, u32 id) {
    if (const u64 sound = R64(g_main + kSoundGot))
        GuestCall(s, g_main + kPlaySound, {sound + 0x168, id});
}
// The cursor on `row`: its index only. The row highlight and info line stay where the layout put them (row 0):
// neither kCursorShow (crashes in the row layout, main+0x7cd420, called from here) nor vfunc +0x1d8 moves them.
void SetRow(State& s, u64 opt, u32 row) {
    const u64 cursor = R64(opt + 0xb0);
    if (!cursor)
        return;
    // the cursor's SetRow (vfunc +0x60, main+0x6bad00: row < its count at +8 -> +0x10 / +0x20)
    GuestCall(s, R64(R64(cursor) + 0x60), {cursor, row});
    W32(opt + kObjCursorRow, row);
    LOG_INFO("Menu", "cursor on row {} (asked {})", CursorRow(s, opt), row);
}
// The current page laid out again (its rows changed), the cursor on `row` (< 0: where the layout puts it).
void Layout(State& s, u64 opt, int row) {
    {
        std::lock_guard lk(g_mutex);
        Show(Cur());
    }
    GuestCall(s, g_main + kFillRows, {opt});
    GuestCall(s, g_main + kRowsDone, {opt});
    HideOthers(s, opt);
    UpdateGrey(s, opt, -1, true);
    if (row >= 0)
        SetRow(s, opt, u32(row));
}

// A on PC SETTINGS' MODS or CONTROLS row: that page in the same slot (tab 2's header named after it, tab 3's hidden).
void EnterSub(State& s, u64 opt, int sub) {
    {
        std::lock_guard lk(g_mutex);
        Apply();
        g_moving = -1;
        g_cur = sub;
        if (sub == kPageMods) {
            g_mods = mods::Scan(false);
            g_mod_first = 0;
            BuildModsRows();
        }
        if (sub == kPageKeys) {
            g_key_first = 0;
            g_key_note_action = -1;
            BuildKeyRows();
        }
    }
    GuestCall(s, g_main + kPlayAnim, {opt + kObjAnims, 0, 0});
    GuestCall(s, g_main + kPlayAnim, {opt + kObjAnims, g_mem->Read<u8>(g_main + kTabAnims + kPageTabs[0]), 0});
    GuestCall(s, g_main + kPlayAnim, {opt + kObjAnims, g_mem->Read<u8>(g_main + kTabOffAnims + kPageTabs[1]), 0});
    GuestCall(s, g_main + kTabNames, {opt});
    Layout(s, opt, -1);
    PlaySound(s, kSoundTab);
    if (sub == kPageMods)
        LOG_INFO("Menu", "MODS page shown ({} mods)", g_mods.size());
    else
        LOG_INFO("Menu", "{} page shown", sub == kPageKeys ? "KEY BINDINGS" : "CONTROLS");
}
// B on MODS or CONTROLS: back to PC SETTINGS; on KEY BINDINGS: back to CONTROLS.
void LeaveSub(State& s, u64 opt) {
    const int sub = g_cur;
    if (sub == kPageKeys) {
        if (keybinds::Capturing() >= 0)
            return;
        {
            std::lock_guard lk(g_mutex);
            g_cur = kPageControls;
        }
        GuestCall(s, g_main + kPlayAnim, {opt + kObjAnims, 0, 0});
        GuestCall(s, g_main + kPlayAnim, {opt + kObjAnims, g_mem->Read<u8>(g_main + kTabAnims + kPageTabs[0]), 0});
        GuestCall(s, g_main + kTabNames, {opt});
        Layout(s, opt, -1);
        PlaySound(s, kSoundBack);
        LOG_INFO("Menu", "KEY BINDINGS page closed, back to CONTROLS");
        return;
    }
    {
        std::lock_guard lk(g_mutex);
        Apply();
        g_moving = -1;
        g_cur = 1;
    }
    const u64 tabs = R64(opt + kObjTabLayout);
    for (u32 p = 0; p < kPanesPerTab; p++) // tab 3's header, hidden on the sub page
        if (const u64 pane = tabs ? GuestCall(s, g_main + kPane, {tabs, kTabPanes + kPanesPerTab * kPageTabs[1] + p}) : 0)
            GuestCall(s, g_main + kSetVisible, {pane, 1});
    GuestCall(s, g_main + kPlayAnim, {opt + kObjAnims, 0, 0});
    GuestCall(s, g_main + kPlayAnim, {opt + kObjAnims, g_mem->Read<u8>(g_main + kTabAnims + kPageTabs[1]), 0});
    GuestCall(s, g_main + kPlayAnim, {opt + kObjAnims, g_mem->Read<u8>(g_main + kTabOffAnims + kPageTabs[0]), 0});
    GuestCall(s, g_main + kTabNames, {opt});
    Layout(s, opt, -1); // (the cursor on the top row, highlight and all, as after a tab switch)
    PlaySound(s, kSoundBack);
    LOG_INFO("Menu", "{} page closed, back to PC SETTINGS", sub == kPageMods ? "MODS" : "CONTROLS");
}
// L / R on MODS: the previous / next 7 mods.
void ModsPage(State& s, u64 opt, int dir) {
    {
        std::lock_guard lk(g_mutex);
        if (g_mods.size() <= kModRows)
            return GuestReturn(s, 0);
        Apply();
        const size_t pages = (g_mods.size() + kModRows - 1) / kModRows;
        g_mod_first = (g_mod_first / kModRows + pages + dir) % pages * kModRows;
        g_moving = -1;
        BuildModsRows();
    }
    GuestCall(s, g_main + kTabNames, {opt});
    Layout(s, opt, -1);
    PlaySound(s, kSoundTab);
    GuestReturn(s, 0);
}
// Y on a mod: picks it up / puts it down.
void PickUp(State& s, u64 opt, u32 row) {
    {
        std::lock_guard lk(g_mutex);
        Apply();
        const int i = int(g_mod_first + row - 1);
        g_moving = g_moving == i ? -1 : i;
        BuildModsRows();
    }
    Layout(s, opt, int(row));
    PlaySound(s, kSoundTab);
}
// Left / right with a mod picked up: one place up / down the load order (the page follows it).
void MoveMod(State& s, u64 opt, int dir) {
    int row = -1;
    {
        std::lock_guard lk(g_mutex);
        const int i = g_moving, j = i + dir;
        if (j < 0 || j >= int(g_mods.size()))
            return;
        Apply();
        std::swap(g_mods[i], g_mods[j]);
        g_moving = j;
        if (size_t(j) < g_mod_first || size_t(j) >= g_mod_first + kModRows)
            g_mod_first = size_t(j) / kModRows * kModRows;
        if (!mods::SaveOrder(g_mods))
            LOG_WARN("Menu", "could not save mods\\order.txt");
        BuildModsRows();
        row = int(1 + j - g_mod_first);
    }
    GuestCall(s, g_main + kTabNames, {opt}); // the page number
    Layout(s, opt, row);
    PlaySound(s, kSoundTab);
}

// The KEY BINDINGS rows' texts again (the value texts redrawn in place, the cursor stays).
void RefreshKeys(State& s, u64 opt) {
    size_t n = 0;
    {
        std::lock_guard lk(g_mutex);
        BuildKeyRows();
        n = Cur().rows.size();
    }
    for (u32 r = 0; r < n; r++)
        GuestCall(s, g_main + kShowValue, {opt, r});
}
// A on KEY BINDINGS: RESET ALL, or the action takes the next key; the tap of A that ends a capture (keybinds::Feed)
// shows its result.
void KeysDecide(State& s, u64 opt, u32 row) {
    namespace kb = keybinds;
    kb::Outcome o;
    if (kb::TakeDone(o)) {
        g_key_note_action = -1;
        if (!o.cancelled) {
            if (o.swapped >= 0) {
                g_key_note_action = o.action;
                g_key_note = u"Swapped: " + kb::Label(kb::Action(o.swapped)) + u" now uses " +
                             kb::Text(kb::Get(kb::Action(o.swapped))) + u".\r\nY: back to the default (" +
                             kb::Text(kb::Default(kb::Action(o.action))) + u").";
            }
            SaveKeys();
            const kb::Binding b = kb::Get(kb::Action(o.action));
            LOG_INFO("Menu", "{} bound to key '{}', mouse '{}'{}", kb::Id(kb::Action(o.action)), kb::KeyName(b.key),
                     kb::MouseName(b.mouse), o.swapped >= 0 ? fmt::format(" (swapped with {})", kb::Id(kb::Action(o.swapped))) : "");
        }
        RefreshKeys(s, opt);
        PlaySound(s, o.cancelled ? kSoundBack : kSoundTab);
        return;
    }
    if (kb::Capturing() >= 0 || R32(opt + kObjState) != kStateIdle)
        return;
    g_key_note_action = -1;
    if (row == 0) {
        kb::ResetAll();
        SaveKeys();
        LOG_INFO("Menu", "key bindings reset");
    } else if (row < Cur().rows.size()) {
        kb::StartCapture(kb::Action(g_key_first + row - 1));
        LOG_INFO("Menu", "binding {}: waiting for a key", kb::Id(kb::Action(g_key_first + row - 1)));
    }
    RefreshKeys(s, opt);
    PlaySound(s, kSoundTab);
}
// L / R on KEY BINDINGS: the previous / next 7 actions.
void KeysPage(State& s, u64 opt, int dir) {
    if (keybinds::Capturing() >= 0)
        return GuestReturn(s, 0);
    {
        std::lock_guard lk(g_mutex);
        const u32 pages = (keybinds::kActions + kKeyRows - 1) / kKeyRows;
        g_key_first = (g_key_first / kKeyRows + pages + dir) % pages * kKeyRows;
        g_key_note_action = -1;
        BuildKeyRows();
    }
    GuestCall(s, g_main + kTabNames, {opt});
    Layout(s, opt, -1);
    PlaySound(s, kSoundTab);
    GuestReturn(s, 0);
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
    if (g_cur == kPageKeys) {
        KeysDecide(s, opt, row);
        return GuestReturn(s, 0);
    }
    if (R32(opt + kObjState) == kStateIdle) {
        if (g_cur == 1 && (row == kRowMods || row == kRowControls)) {
            EnterSub(s, opt, row == kRowMods ? kPageMods : kPageControls);
            return GuestReturn(s, 0);
        }
        if (g_cur == kPageControls && row == kRowKeys) {
            EnterSub(s, opt, kPageKeys);
            return GuestReturn(s, 0);
        }
        if (g_cur == kPageMods && g_moving >= 0 && row >= 1 && int(g_mod_first + row - 1) == g_moving) {
            PickUp(s, opt, row);
            return GuestReturn(s, 0);
        }
    }
    if (row < Cur().rows.size() && Cur().rows[row].type != kButton && !R32(opt + kObjRowOff + 4 * row)) {
        u8 v[16];
        g_mem->ReadBlock(Cur().values, v, sizeof(v));
        v[row] = u8((v[row] + 1) % Cur().rows[row].choices.size());
        g_mem->WriteBlock(Cur().values, v, sizeof(v));
        GuestCall(s, g_main + kShowValue, {opt, row});
        Exclude(s, opt, row, v[row]);
    }
    GuestReturn(s, 0);
}

// B on our pages opened from the GRAPHICS row: back to CONTROLS (the cursor on its first row, as on a tab switch).
void Back(State& s) {
    const u64 opt = s.x[0];
    if (OnPage(opt) && SubPage()) {
        if (R32(opt + kObjState) == kStateIdle)
            LeaveSub(s, opt);
        return GuestReturn(s, 0);
    }
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

// A row's value changed to `value` (on the game's thread; an arrow row may store it only at the end of its
// animation): FSR and SSAA exclude each other (picking FSR turns SSAA into FXAA, picking SSAA turns FSR off), and
// FSR SHARPNESS follows the UPSCALER.
void Exclude(State& s, u64 opt, u32 row, u8 value) {
    if (!OnPage(opt) || g_cur != 0 || (row != kRowUpscaler && row != kRowAa))
        return;
    const u64 v = Cur().values;
    u8 fsr = row == kRowUpscaler ? value : g_mem->Read<u8>(v + kRowUpscaler);
    const u8 aa = row == kRowAa ? value : g_mem->Read<u8>(v + kRowAa);
    if (fsr != 0 && aa >= kAaSsaa2) {
        const u32 other = row == kRowUpscaler ? kRowAa : kRowUpscaler;
        g_mem->Write<u8>(v + other, other == kRowAa ? u8(kAaFxaa) : u8(0));
        GuestCall(s, g_main + kShowValue, {opt, other});
        if (other == kRowUpscaler)
            fsr = 0;
    }
    UpdateGrey(s, opt, fsr);
}
// Left / right on a row: the row's new value (read back, or the step the game makes: arrows wrap round).
void Step(State& s, Hook h, int dir) {
    const u64 opt = s.x[0];
    if (OnPage(opt) && g_cur == kPageMods && g_moving >= 0) {
        if (R32(opt + kObjState) == kStateIdle)
            MoveMod(s, opt, dir);
        return GuestReturn(s, 0);
    }
    const u32 row = OnPage(opt) ? CursorRow(s, opt) : ~0u;
    const bool ours = row < Cur().rows.size() && !R32(opt + kObjRowOff + 4 * row);
    const u8 before = ours ? g_mem->Read<u8>(Cur().values + row) : 0;
    RunOriginal(s, g_hooks[h]);
    if (!ours)
        return;
    u8 now = g_mem->Read<u8>(Cur().values + row);
    if (now == before) {
        const int n = int(Cur().rows[row].choices.size());
        now = u8(n ? (before + dir + n) % n : before);
    }
    Exclude(s, opt, row, now);
}
void ValuePrev(State& s) { Step(s, kHValuePrev, -1); }
void ValueNext(State& s) { Step(s, kHValueNext, 1); }

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
    if (g_cur == kPageKeys) {
        if (row >= 1 && row < Cur().rows.size() && keybinds::Capturing() < 0 && R32(opt + kObjState) == kStateIdle) {
            const auto a = keybinds::Action(g_key_first + row - 1);
            keybinds::Set(a, keybinds::Default(a));
            g_key_note_action = -1;
            SaveKeys();
            RefreshKeys(s, opt);
            PlaySound(s, kSoundTab);
        }
        return GuestReturn(s, 0);
    }
    if (g_cur == kPageMods && row >= 1 && row < Cur().rows.size() && !g_mods.empty()) {
        if (R32(opt + kObjState) == kStateIdle)
            PickUp(s, opt, row);
        return GuestReturn(s, 0);
    }
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
        Exclude(s, opt, row, v[row]);
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
        const u32 title = g_pages[h == kStrControls ? (SubPage() ? g_cur : 0) : 1].title; // headers
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
    keybinds::Load(core::Sys().CfgSnapshot());
    prompts::Init(); // (button icons per input device: no game code involved)
    // the build these offsets come from: the first words of the functions, the row counts and tab 2's row types
    static constexpr std::pair<u64, u32> kExpect[] = {
        {kOptInit, 0xf81d0ff5},   {kOptStore, 0xa9be4ff4},  {kChoiceCount, 0xb9419808}, {kTabPrev, 0xa9bd57f6},
        {kTabNext, 0xa9bd57f6},   {kValuePrev, 0xa9bc5ff8}, {kValueNext, 0xa9bc5ff8},   {kDecide, 0xf81c0ff7},
        {kDefault, 0xa9bd57f6},   {kShowValue, 0xfc1b0fe8},
        {kFillRows, 0xfc170fec},  {kRowsDone, 0xfc1c0fea},  {kGetString, 0x2a0103e8},  {kRowCounts + 4, 0x40003},
        {kRowTypes, 1},           {kRowTypes + 4 * kEntryRow, 0}, {kBack, 0xf81e0ff3}, {kTabNames, 0xfc1a0fe8},
        {kGreyRow, 0xfc1c0fe8},   {kUngreyRow, 0xfc1c0fe8}, {kCursorShow, 0xfc1b0fea},
    };
    for (auto [off, word] : kExpect)
        if (!mem.IsValid(main_base + off, 4) || mem.Read<u32>(main_base + off) != word) {
            LOG_WARN("Menu", "unknown game build (main+{:#x}): graphics page off", off);
            return;
        }
    g_mem = &mem;
    g_main = main_base;
    g_launch_mods_enabled = core::Sys().CfgSnapshot().mods_enabled;
    BuildPages();
    g_entry_info = Text(u"Display mode, resolution, frame rate and anti-aliasing. R: PC SETTINGS.");
    const std::pair<u64, void (*)(State&)> hooks[kHookCount] = {
        {kOptInit, OptInit},     {kOptStore, OptStore},   {kChoiceCount, ChoiceCount}, {kTabPrev, TabPrev},
        {kTabNext, TabNext},     {kDecide, Decide},       {kDefault, Default},         {kGetString, GetString},
        {kBack, Back},           {kValuePrev, ValuePrev}, {kValueNext, ValueNext},
    };
    for (int i = 0; i < kHookCount; i++) {
        g_hooks[i].pc = main_base + hooks[i].first;
        g_hooks[i].host = hooks[i].second;
        g_hooks[i].direct = i == kHStore || i == kHCount || i == kHString; // also called with BL
        kernel.Cpus().Override(g_hooks[i]);
    }
    std::thread(Watch).detach();
    LOG_INFO("Menu", "graphics pages installed ({} + {} + {} rows, {} of {} KB of texts)", g_pages[0].rows.size(),
             g_pages[1].rows.size(), g_pages[kPageControls].rows.size(), (g_alloc - g_block) / 1024, kBlockSize / 1024);
}

} // namespace hle::menu::graphics
