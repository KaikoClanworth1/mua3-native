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
// The GRAPHICS page is tab 2 with its data replaced while the page is open: our row table, row types, value bytes
// and choice counts, our text for hashes 0xFE00xxxx, the tab named GRAPHICS (the game's own string) and the other
// tabs hidden. L / R and A do nothing there. Every change is applied at once (core::System::UpdateConfig): what
// can change while the game runs changes now, the rest is saved to mua3.ini for the next start. Closing the page
// puts the tab back and leaves the game's own settings as they were.
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
constexpr u64 kPlayAnim = 0x5e4d00;                   // (animation helper, animation, 0)
constexpr u64 kPane = 0x5f39d0;                       // (layout, pane index) -> pane
constexpr u64 kSetVisible = 0x12f5d40;                // (pane, visible)
constexpr u64 kGetString = 0x1d6290;                  // (string table, hash) -> const char16_t* (direct)
constexpr u64 kRowsGot = 0x218bd40, kValuesGot = 0x218bd20; // tab 2's row table and value bytes (GOT entries)
constexpr u64 kRowCounts = 0x1a634b2;                 // u16[5]
constexpr u64 kRowTypes = 0x1a634ec;                  // u32[] tab 2 (the next table, the language row's, is tab 0's)
constexpr u64 kTabAnims = 0x1a634a8;                  // u8[5]: tab_0N_ON animation
constexpr u32 kTab = 2;
// options object layout
constexpr u64 kObjTab = 0x198, kObjAnims = 0xc0, kObjTabLayout = 0xc8;
// 09_option_tub_menu panes: tab N (0-4) = Window_tab_0N, Window_tab_ON_0N, tab_name_0N at 1 + 3N..; L/R icons 16-19
constexpr u32 kTabPanes = 1, kPanesPerTab = 3, kButtonPanes = 16, kButtonPaneCount = 4;
// system string hashes
constexpr u32 kStrControls = 0x023fd6, kStrGraphics = 0x36c4b2, kStrOn = 0x3a32fb, kStrOff = 0x19edbe,
              kStrAuto = 0x0855d9;
constexpr u32 kOurs = 0xFE000000; // our strings: kOurs + index (the game's hashes are 22-bit)

enum RowType : u32 { kTwoChoices = 1, kArrows = 2 };

constexpr std::pair<int, int> kResolutions[] = {{1280, 720},  {1600, 900},  {1920, 1080}, {2560, 1440}, {3200, 1800},
                                                {3840, 2160}, {2560, 1080}, {3440, 1440}, {5120, 2880}};
constexpr int kRates[] = {30, 60, 72, 90, 120, 144, 165, 240}; // as the launcher offers them
constexpr int kLimits[] = {0, 30, 60, 72, 90, 120, 144, 165, 240};

enum Rows { kRowMode, kRowResolution, kRowScale, kRowRate, kRowLimit, kRowVsync, kRowShowFps, kRowAa, kRowCount };

core::Memory* g_mem = nullptr;
u64 g_main = 0;

// guest memory: strings, then the row table, choice arrays and value bytes
u64 g_block = 0, g_alloc = 0;
constexpr u64 kBlockSize = 0x8000;
std::vector<u64> g_strings;     // our string index -> guest address
u64 g_custom_res_text = 0;      // "W x H" for a window size that is no preset (rewritten on open)
u64 g_custom_limit_text = 0;    // "N" for an fps limit that is no preset
u64 g_rows = 0, g_values = 0;
struct RowInfo {
    u32 label, info;
    RowType type;
    std::vector<u32> choices; // hashes
    u64 guest_choices = 0;
};
std::vector<RowInfo> g_row_info;

std::mutex g_mutex;
std::atomic<bool> g_open_next{false}, g_active{false};
u64 g_page = 0; // the options object showing the page
u64 g_saved_rows = 0, g_saved_values = 0;
u16 g_saved_count = 0;
u32 g_saved_types[kRowCount]{};

enum Hook {
    kHInit, kHStore, kHCount, kHTabPrev, kHTabNext, kHDecide, kHDefault, kHString, kHookCount
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

u32 g_custom_res = 0, g_custom_limit = 0; // hashes of the two rewritable strings

void BuildPage() {
    g_block = g_mem->FindFree(core::layout::kKernelMapBase, 0x1000000000ull, kBlockSize);
    g_mem->Map(g_block, kBlockSize, core::MS_Normal, core::MP_RW);
    g_alloc = g_block;
    auto add = [](u32 label, u32 info, RowType type, std::vector<u32> choices) {
        g_row_info.push_back({label, info, type, std::move(choices)});
    };
    add(Text(u"DISPLAY MODE"), Text(u"Play in a window or fullscreen. F11 also switches."), kTwoChoices,
        {Text(u"WINDOWED"), Text(u"FULLSCREEN")});
    std::vector<u32> res;
    for (auto [w, h] : kResolutions)
        res.push_back(Text(U16(fmt::format("{} x {}", w, h))));
    g_custom_res = Text(u"", 24);
    res.push_back(g_custom_res);
    add(Text(u"RESOLUTION"), Text(u"The window's size in windowed mode.\r\nFullscreen uses the monitor's resolution."),
        kArrows, res);
    add(Text(u"RENDER SCALE"),
        Text(u"The resolution the game renders at. AUTO matches your display.\r\nApplies after a restart."), kArrows,
        {kStrAuto, Text(u"1X (1080P)"), Text(u"2X (1440P / 4K)"), Text(u"3X"), Text(u"4X")});
    std::vector<u32> rates;
    for (int r : kRates)
        rates.push_back(Text(U16(r == 30 ? "30 (ORIGINAL)" : r > 60 ? fmt::format("{} (EXPERIMENTAL)", r)
                                                                   : std::to_string(r))));
    add(Text(u"FRAME RATE"),
        Text(u"The game's frame rate. 30 is the original, 60 is fully supported.\r\nApplies after a restart."),
        kArrows, rates);
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
    add(Text(u"ANTI-ALIASING"), Text(u"Smooths jagged edges. FXAA is fast and soft."), kArrows,
        {kStrOff, Text(u"FXAA")});
    g_custom_res_text = g_strings[g_custom_res - kOurs];
    g_custom_limit_text = g_strings[g_custom_limit - kOurs];
    // the row table and the value bytes
    for (RowInfo& r : g_row_info) {
        r.guest_choices = Alloc(4 * r.choices.size());
        for (size_t i = 0; i < r.choices.size(); i++)
            W32(r.guest_choices + 4 * i, r.choices[i]);
    }
    g_rows = Alloc(16 * g_row_info.size());
    for (size_t i = 0; i < g_row_info.size(); i++) {
        W32(g_rows + 16 * i, g_row_info[i].label);
        W32(g_rows + 16 * i + 4, g_row_info[i].info);
        W64(g_rows + 16 * i + 8, g_row_info[i].guest_choices);
    }
    g_values = Alloc(16);
}

template <size_t N, typename T> int IndexOf(const T (&list)[N], const T& v, int otherwise) {
    for (size_t i = 0; i < N; i++)
        if (list[i] == v)
            return int(i);
    return otherwise;
}

// The settings as value bytes.
void ToPage(const core::Config& c) {
    u8 v[kRowCount]{};
    v[kRowMode] = c.fullscreen ? 1 : 0;
    const int res = IndexOf(kResolutions, std::pair{c.width, c.height}, int(std::size(kResolutions)));
    const std::u16string custom = U16(fmt::format("{} x {}", c.width, c.height));
    g_mem->WriteBlock(g_custom_res_text, custom.c_str(), (custom.size() + 1) * 2);
    v[kRowResolution] = u8(res);
    v[kRowScale] = u8(std::clamp(c.resolution_scale, 0, 4));
    v[kRowRate] = u8(IndexOf(kRates, c.fps_target, 1));
    const std::u16string limit = U16(std::to_string(c.fps_limit));
    g_mem->WriteBlock(g_custom_limit_text, limit.c_str(), (limit.size() + 1) * 2);
    v[kRowLimit] = u8(IndexOf(kLimits, c.fps_limit, int(std::size(kLimits))));
    v[kRowVsync] = c.vsync ? 0 : 1;
    v[kRowShowFps] = c.show_fps ? 0 : 1;
    v[kRowAa] = u8(std::clamp(c.anti_aliasing, 0, 1));
    g_mem->WriteBlock(g_values, v, sizeof(v));
}

// The value bytes as settings: `running` = only what can change while the game runs.
void FromPage(core::Config& c, bool running) {
    u8 v[kRowCount];
    g_mem->ReadBlock(g_values, v, sizeof(v));
    c.fullscreen = v[kRowMode] == 1;
    if (v[kRowResolution] < std::size(kResolutions))
        std::tie(c.width, c.height) = kResolutions[v[kRowResolution]];
    if (v[kRowLimit] < std::size(kLimits))
        c.fps_limit = kLimits[v[kRowLimit]];
    c.vsync = v[kRowVsync] == 0;
    c.show_fps = v[kRowShowFps] == 0;
    c.anti_aliasing = std::min<int>(v[kRowAa], 1);
    if (running)
        return;
    c.resolution_scale = std::min<int>(v[kRowScale], 4);
    if (v[kRowRate] < std::size(kRates))
        c.fps_target = kRates[v[kRowRate]];
}

u8 g_applied[kRowCount]{}; // the value bytes last applied
void Apply(bool always = false) {
    u8 v[kRowCount];
    g_mem->ReadBlock(g_values, v, sizeof(v));
    if (!always && std::memcmp(v, g_applied, sizeof(v)) == 0)
        return;
    std::memcpy(g_applied, v, sizeof(v));
    core::Sys().UpdateConfig([](core::Config& c, bool running) { FromPage(c, running); });
}

// ---- hooks
void Hide(State& s, u64 layout, u32 pane) {
    if (const u64 p = layout ? GuestCall(s, g_main + kPane, {layout, pane}) : 0)
        GuestCall(s, g_main + kSetVisible, {p, 0});
}

// Opening the OPTIONS screen: as the GRAPHICS page if asked for.
void OptInit(State& s) {
    const u64 opt = s.x[0];
    RunOriginal(s, g_hooks[kHInit]);
    if (!g_open_next.exchange(false))
        return;
    {
        std::lock_guard lk(g_mutex);
        if (g_active) // not closed the usual way: the tables still hold the page
            return;
        g_saved_rows = R64(g_main + kRowsGot);
        g_saved_values = R64(g_main + kValuesGot);
        g_saved_count = g_mem->Read<u16>(g_main + kRowCounts + 2 * kTab);
        for (u32 i = 0; i < kRowCount; i++)
            g_saved_types[i] = R32(g_main + kRowTypes + 4 * i);
        W64(g_main + kRowsGot, g_rows);
        W64(g_main + kValuesGot, g_values);
        g_mem->Write<u16>(g_main + kRowCounts + 2 * kTab, u16(kRowCount));
        for (u32 i = 0; i < kRowCount; i++)
            W32(g_main + kRowTypes + 4 * i, g_row_info[i].type);
        ToPage(core::Sys().CfgSnapshot());
        g_mem->ReadBlock(g_values, g_applied, sizeof(g_applied));
        g_page = opt;
        g_active = true;
    }
    // show tab 2 (named GRAPHICS by the string hook), alone
    g_mem->Write<u32>(opt + kObjTab, kTab);
    GuestCall(s, g_main + kPlayAnim, {opt + kObjAnims, g_mem->Read<u8>(g_main + kTabAnims + kTab), 0});
    GuestCall(s, g_main + kFillRows, {opt});
    GuestCall(s, g_main + kRowsDone, {opt});
    const u64 tabs = R64(opt + kObjTabLayout);
    for (u32 t = 0; t < 5; t++)
        if (t != kTab)
            for (u32 p = 0; p < kPanesPerTab; p++)
                Hide(s, tabs, kTabPanes + kPanesPerTab * t + p);
    for (u32 p = 0; p < kButtonPaneCount; p++)
        Hide(s, tabs, kButtonPanes + p);
    LOG_INFO("Menu", "Graphics page opened");
}

// Closing: the page puts the tab back instead of storing the game's settings.
void OptStore(State& s) {
    {
        std::lock_guard lk(g_mutex);
        if (!g_active || s.x[0] != g_page)
            return RunOriginal(s, g_hooks[kHStore]);
        Apply(true);
        W64(g_main + kRowsGot, g_saved_rows);
        W64(g_main + kValuesGot, g_saved_values);
        g_mem->Write<u16>(g_main + kRowCounts + 2 * kTab, g_saved_count);
        for (u32 i = 0; i < kRowCount; i++)
            W32(g_main + kRowTypes + 4 * i, g_saved_types[i]);
        g_active = false;
        g_page = 0;
    }
    LOG_INFO("Menu", "Graphics page closed");
    GuestReturn(s, 0);
}

bool OnPage(u64 opt) { return g_active.load(std::memory_order_acquire) && opt == g_page; }

void ChoiceCount(State& s) {
    if (!OnPage(s.x[0]) || R32(s.x[0] + kObjTab) != kTab)
        return RunOriginal(s, g_hooks[kHCount]);
    const u32 row = u32(s.x[1]);
    GuestReturn(s, row < g_row_info.size() ? g_row_info[row].choices.size() : 0);
}

void Blocked(State& s, Hook h) {
    if (OnPage(s.x[0]))
        return GuestReturn(s, 0);
    RunOriginal(s, g_hooks[h]);
}
void TabPrev(State& s) { Blocked(s, kHTabPrev); }
void TabNext(State& s) { Blocked(s, kHTabNext); }
void Decide(State& s) { Blocked(s, kHDecide); }

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
    if (!OnPage(opt))
        return RunOriginal(s, g_hooks[kHDefault]);
    const u64 cursor = R64(opt + 0xb0);
    const u32 row = cursor ? u32(GuestCall(s, R64(R64(cursor) + 0x58), {cursor})) : ~0u;
    if (row < kRowCount) {
        core::Config defaults;
        defaults.fps_limit = 0; // the launcher's presets: no cap
        u8 v[kRowCount];
        g_mem->ReadBlock(g_values, v, sizeof(v));
        ToPage(defaults);
        u8 d[kRowCount];
        g_mem->ReadBlock(g_values, d, sizeof(d));
        v[row] = d[row];
        ToPage(core::Sys().CfgSnapshot()); // the custom value texts
        g_mem->WriteBlock(g_values, v, sizeof(v));
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
    } else if (h == kStrControls && g_active.load(std::memory_order_relaxed)) {
        s.x[1] = kStrGraphics; // the page's tab
    }
    RunOriginal(s, g_hooks[kHString]);
}

} // namespace

void OpenNext() { g_open_next = true; }

u32 LabelHash() { return kStrGraphics; }

void Install(core::Memory& mem, kernel::Kernel& kernel, u64 main_base) {
    // the build these offsets come from: the first words of the functions, the row counts and tab 2's row types
    static constexpr std::pair<u64, u32> kExpect[] = {
        {kOptInit, 0xf81d0ff5},   {kOptStore, 0xa9be4ff4},  {kChoiceCount, 0xb9419808}, {kTabPrev, 0xa9bd57f6},
        {kTabNext, 0xa9bd57f6},   {kValuePrev, 0xa9bc5ff8}, {kValueNext, 0xa9bc5ff8},   {kDecide, 0xf81c0ff7},
        {kDefault, 0xa9bd57f6},   {kShowValue, 0xfc1b0fe8},
        {kFillRows, 0xfc170fec},  {kRowsDone, 0xfc1c0fea},  {kGetString, 0x2a0103e8},  {kRowCounts + 4, 0x40003},
        {kRowTypes, 1},
    };
    for (auto [off, word] : kExpect)
        if (!mem.IsValid(main_base + off, 4) || mem.Read<u32>(main_base + off) != word) {
            LOG_WARN("Menu", "unknown game build (main+{:#x}): graphics page off", off);
            return;
        }
    g_mem = &mem;
    g_main = main_base;
    BuildPage();
    const std::pair<u64, void (*)(State&)> hooks[kHookCount] = {
        {kOptInit, OptInit},     {kOptStore, OptStore},   {kChoiceCount, ChoiceCount}, {kTabPrev, TabPrev},
        {kTabNext, TabNext},     {kDecide, Decide},       {kDefault, Default},         {kGetString, GetString},
    };
    for (int i = 0; i < kHookCount; i++) {
        g_hooks[i].pc = main_base + hooks[i].first;
        g_hooks[i].host = hooks[i].second;
        g_hooks[i].direct = i == kHStore || i == kHCount || i == kHString; // also called with BL
        kernel.Cpus().Override(g_hooks[i]);
    }
    std::thread(Watch).detach();
    LOG_INFO("Menu", "graphics page installed ({} rows)", g_row_info.size());
}

} // namespace hle::menu::graphics
