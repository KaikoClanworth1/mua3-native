// Extensions to the game's main menu (the menu opened with + during play, and at S.H.I.E.L.D. points).
//
// How the game builds it (main module, 4.0.1; full notes in docs/game_menus.md):
//  - One class (vtable main+0x2109f88) drives both the pause menu and the S.H.I.E.L.D. point menu. It knows 11
//    fixed item slots: 0 TEAM, 1 HERO, 2 INFINITY MISSION, 3 LAB, 4 CO-OP, 5 SYSTEM (main list) and 6 SAVE, 7 LOAD,
//    8 OPTIONS, 9 TUTORIAL, 10 QUIT GAME (SYSTEM submenu). Each slot owns an instance of the 01_savepoint_parts
//    layout (this+0xf8+8*slot).
//  - Refresh (vfunc +0x448, main+0x6e2e90) rebuilds the lists: for every slot, GetItemState (+0x460, 0x6e3150:
//    0 shown, 1 grey, 2 hidden) and IsSystemItem (+0x490) decide whether and where it appears; shown slots are
//    appended in slot order to the main list (vector of 20-byte entries at this+0xc8) or the system list (+0xe0),
//    and SetItemAppearance (+0x480, 0x6e31f0) puts the slot's label on its parts layout. Labels are pre-rendered
//    per-language textures (GText_*) indexed through three 11-entry tables (off main+0x1a3cb20, grey +0x1a3cb4c,
//    on +0x1a3cb78). Finally the enabled flag of each list position is copied to this+0x180 (main, 6) / +0x198.
//  - List position i is drawn at layout pane Null_0<i+1>_pos (01_savepoint: 6 main and 5 system positions).
//  - Choosing an item runs ExecuteItem(this, slot) (main+0x6e0890, a switch over the 11 slots). QUIT GAME shows a
//    yes/no dialog through the dialog manager (main+0x628940, a 0x60-byte parameter block) whose callback
//    (main+0x6e2c30) returns to the title screen on "OKAY".
//
// The slot count is fixed by the class layout (the 11 parts pointers end where other members start), so a new item
// borrows a main-list slot the game hides in the current context (HERO / INFINITY MISSION / LAB are hidden in the
// pause menu), gets its own label and is moved to the bottom of the list; ExecuteItem runs its action instead of
// the borrowed slot's. Added this way:
//  - QUIT GAME at the bottom of the pause menu (MUA3_PAUSE_QUIT=1): the game's own label and dialog, asking to quit
//    to the desktop; "OKAY" closes the game (the SYSTEM submenu's QUIT GAME still returns to the title screen).
//  - GRAPHICS in the pause menu's SYSTEM submenu, right after OPTIONS: SAVE or LOAD (hidden there) is borrowed; its
//    label is drawn over GText_HERO_gray_out / GText_INFINITY_MISSION_gray_out (HERO and INFINITY MISSION are never
//    greyed out in this menu: their state is shown or hidden), and it opens OPTIONS as the GRAPHICS page. In the
//    S.H.I.E.L.D. point menu all five submenu positions are taken (and the cursor code stops at five), so TIPS (still
//    in the pause menu) is borrowed there.
#include "core/hle/menu_mods.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <span>
#include <mutex>
#include <string>
#include <unordered_map>

#include "core/cpu/cpu.h"
#include "core/cpu/recomp/recomp_rt.h"
#include "core/kernel/kernel.h"
#include "core/memory.h"
#include "core/hle/menu_graphics.h"
#include "core/hle/menu_guest.h"
#include "core/hle/menu_label.h"
#include "core/system.h"

namespace hle::menu {
namespace {

using recomp::State;

// ---- the game's code and data (offsets into main, 4.0.1)
constexpr u64 kMenuVtable = 0x2109f88;
constexpr u64 kRefresh = 0x6e2e90;           // vfunc +0x448 (this)
constexpr u64 kGetItemState = 0x6e3150;      // vfunc +0x460 (this, slot) -> 0 shown, 1 grey, 2 hidden
constexpr u64 kSetItemAppearance = 0x6e31f0; // vfunc +0x480 (this, slot, state)
constexpr u64 kIsSystemItem = 0x7e33f0;      // vfunc +0x490 (this, slot): which list, and which panes draw it
constexpr u64 kExecuteItem = 0x6e0890;       // (this, slot), called directly by the menu's state machine
constexpr u64 kStartShop = 0x840850;         // (shop state, out) -> 1 if the eShop applet was started, called directly
constexpr u64 kShopOrNotice = 0x6e57c0;      // (a, b): the eShop, or a notice if it cannot open, called directly
// return addresses: the title menu's "Nintendo eShop" item calls StartShop (or, without a usable account,
// ShopOrNotice); a 0 result leaves the menu as it is
constexpr u64 kTitleShopRet = 0x720f38, kTitleNoticeRet = 0x721000;
constexpr u64 kQuitCallback = 0x6e2c30;      // dialog callback (button): 0 = OKAY
constexpr u64 kShowDialog = 0x628940;        // (dialog manager, const params[0x60], owner)
constexpr u64 kGetString = 0x1d6290;         // (string table, label hash) -> const char16_t*
constexpr u64 kStringTableVar = 0x2187ce8;   // GOT entry: address of the system string table pointer
constexpr u64 kUiRootVar = 0x2187dc0;        // GOT entry: address of the UI root pointer (+0x30 dialog manager)
constexpr u64 kTexOff = 0x1a3cb20, kTexGrey = 0x1a3cb4c, kTexOn = 0x1a3cb78; // u32[11] each, -1 = none
// mes_system label hashes (UI/string/hash_table/hash_table_system.bin)
constexpr u32 kStrOkay = 0x3a471d, kStrCancel = 0x0cb958, kStrSelectHint = 0x3303db;
// 01_savepoint_parts texture indices (alphabetical GText_* list of every language's texture pack)
constexpr u32 kTexQuitGame = 19, kTexQuitGameOn = 20;
constexpr u32 kTexHeroGrey = 3, kTexInfinityGrey = 6; // never shown by this menu: drawn as GRAPHICS (off / on)
// menu object layout
constexpr u64 kMainListBegin = 0xc8, kMainListEnd = 0xd0, kMainFlags = 0x180;
constexpr u64 kRestricted = 0x17c; // 1 = the pause menu (HERO, INFINITY MISSION, LAB, SAVE and LOAD hidden)
constexpr u32 kEntrySize = 0x14, kEntryEnabled = 0x10, kMainPositions = 6;
constexpr u32 kSlotSystem = 5, kHidden = 2;
constexpr u64 kSysListBegin = 0xe0, kSysListEnd = 0xe8, kSysFlags = 0x198;
constexpr u32 kSysPositions = 5, kSlotSave = 6, kSlotLoad = 7, kSlotOptions = 8, kSlotTips = 9;

core::Memory* g_mem = nullptr;
u64 g_main = 0;
u64 g_text = 0; // our UTF-16 strings in guest memory

std::mutex g_mutex;
std::unordered_map<u64, u32> g_borrowed; // menu object -> slot shown as QUIT GAME (absent: none)
std::unordered_map<u64, u32> g_title_borrowed; // title menu object -> slot shown as GRAPHICS
std::unordered_map<u64, u32> g_sys_borrowed;   // menu object -> SYSTEM submenu slot shown as GRAPHICS
bool g_pause_quit = false;                     // MUA3_PAUSE_QUIT=1: QUIT GAME in the pause menu too
std::atomic<bool> g_quit_pending{false}; // our dialog is up: its callback decides about the desktop

// the title menu (vtable main+0x210ee80; 16 slots, 11-15 = the SYSTEM submenu: SAVE, LOAD, OPTIONS, TIPS, GALLERY)
constexpr u64 kTitleVtable = 0x210ee80;
constexpr u64 kTitleRefresh = 0x7214d0;    // vfunc +0x448 (this)
constexpr u64 kTitleState = 0x721720;      // vfunc +0x460 (this, slot) -> 0 shown, 1 grey, 2 hidden
constexpr u64 kTitleAppearance = 0x721890; // vfunc +0x480 (this, slot, state)
constexpr u64 kTitleIsSystem = 0x7e4080;   // vfunc +0x490 (this, slot)
constexpr u64 kTitleExecute = 0x720c10;    // vfunc +0x4a8 (this, slot)
constexpr u64 kTitleUpdate = 0x71f760;     // vfunc +0x3f0 (this): every frame, places the items at their panes
constexpr u64 kTitleTexOff = 0x1a3d290, kTitleTexOn = 0x1a3d2d0;       // u32[16] label textures per slot
constexpr u64 kTitleSysCursor = 0x1a3d27c, kTitleSysText = 0x1a3d23c; // u32[5] position panes per system position
constexpr u64 kPane = 0x5f39d0;            // (layout, pane index) -> pane
constexpr u32 kTitleOptions = 13, kTitleSystemPositions = 5;
constexpr u32 kTitleBorrowable[] = {7, 6, 4}; // LAB, HERO, INFINITY: hidden on the title screen (they have labels)
// 10_title_menu_parts textures: GText_MAIN_MENU(_on), used by no item, drawn as GRAPHICS
constexpr u32 kTexMainMenu = 28, kTexMainMenuOn = 29;
// 10_title_menu_02 panes: the SYSTEM submenu's 4th / 5th positions, and the unused 12th main-list position, which is
// moved one step below the 5th for the 6th item
constexpr u32 kCursorSub4 = 45, kCursorSub5 = 46, kCursorSpare = 39; // Null_cursor_sub_04/05_pos, Null_cursor_12_pos
constexpr u32 kTextSub4 = 66, kTextSub5 = 67, kTextSpare = 62;       // Null_GText_sub_04/05_pos, Null_GText_12_pos
// pane object: +0xd0 parent, +0xe0 world transform, +0xe8 local transform (translation at +0x30), +0x104 flags,
// +0x110 colour (RGBA8; the title menu applies a position pane's alpha to the item drawn there)
constexpr u64 kPaneParent = 0xd0, kPaneWorld = 0xe0, kPaneLocal = 0xe8, kPaneFlags = 0x104, kPaneColor = 0x110;
constexpr u32 kPaneDirty = 0x10;

enum Hook {
    kHRefresh, kHState, kHAppearance, kHSystem, kHExecute, kHQuit, kHShop, kHNotice,
    kHTRefresh, kHTState, kHTAppearance, kHTSystem, kHTExecute, kHTUpdate, kHookCount
};
thread_local u32 t_system_style = ~0u; // SetItemAppearance of the borrowed slot is running
cpu::FunctionOverride g_hooks[kHookCount];

u64 R64(u64 a) { return g_mem->Read<u64>(a); }
u32 R32(u64 a) { return g_mem->Read<u32>(a); }
void W32(u64 a, u32 v) { g_mem->Write<u32>(a, v); }

u32 Borrowed(u64 menu) {
    std::lock_guard lk(g_mutex);
    auto it = g_borrowed.find(menu);
    return it == g_borrowed.end() ? ~0u : it->second;
}

u32 SysBorrowed(u64 menu) {
    std::lock_guard lk(g_mutex);
    auto it = g_sys_borrowed.find(menu);
    return it == g_sys_borrowed.end() ? ~0u : it->second;
}

u64 Call(State& s, u64 pc, std::initializer_list<u64> args, const cpu::FunctionOverride* h = nullptr) {
    return GuestCall(s, pc, args, h);
}
void Original(State& s, Hook h) { RunOriginal(s, g_hooks[h]); }
void Return(State& s, u64 v) { GuestReturn(s, v); }
bool IsMenu(u64 obj) { return obj && R64(obj) == g_main + kMenuVtable; }

// ---- messages (the game's style; every character exists in that language's game text, so its font has it)
struct Message {
    const char* lang; // mes_system_<lang>.bin
    const char16_t* text;
};
const Message kQuitToDesktop[] = {
    {"eng", u"Do you want to quit the game and return\r\nto the desktop?"},
    {"fra", u"Quitter le jeu et retourner au bureau ?"},
    {"ger", u"Möchtest du das Spiel beenden\r\nund zum Desktop zurückkehren?"},
    {"ita", u"Vuoi uscire dal gioco e tornare\r\nal desktop?"},
    {"esp", u"¿Quieres salir del juego\r\ny volver al escritorio?"},
    {"jpn", u"ゲームを終了し、デスクトップに戻り"
            u"ます\r\nよろしいですか？"},
    {"kor", u"게임을 끝내고 바탕 화면으로 돌아갑니"
            u"다.\r\n괜찮겠습니까?"},
    {"chn", u"確定要離開遊戲嗎？"},
    {"smp", u"确定要退出游戏并返回\r\n桌面吗？"},
};
u64 g_quit_text[std::size(kQuitToDesktop)];

// The game's text language for the configured system language (am GetDesiredLanguage's mapping).
size_t LanguageIndex() {
    static const char* const kByCode[] = {"jpn", "eng", "fra", "ger", "ita", "esp", "smp", "kor", "eng",
                                          "eng", "eng", "chn", "eng", "fra", "esp", "smp", "chn"};
    const int code = core::Sys().Cfg().language;
    const char* lang = code >= 0 && code < int(std::size(kByCode)) ? kByCode[code] : "eng";
    for (size_t i = 0; i < std::size(kQuitToDesktop); i++)
        if (std::strcmp(kQuitToDesktop[i].lang, lang) == 0)
            return i;
    return 0;
}

// The game's QUIT GAME dialog (as ExecuteItem builds it for slot 10), with our message and callback flag.
void ShowQuitDialog(State& s, u64 menu) { // menu 0: no owner (the title screen)
    const u64 root = R64(R64(g_main + kUiRootVar));
    const u64 dialogs = root ? R64(root + 0x30) : 0;
    const u64 strings = R64(R64(g_main + kStringTableVar));
    if (!dialogs || !strings) {
        LOG_WARN("Menu", "no dialog manager: quitting without asking");
        core::Sys().RequestExit();
        return;
    }
    const u64 okay = Call(s, g_main + kGetString, {strings, kStrOkay});
    const u64 cancel = Call(s, g_main + kGetString, {strings, kStrCancel});
    const u64 hint = Call(s, g_main + kGetString, {strings, kStrSelectHint});
    const u64 owner = menu ? Call(s, R64(R64(menu) + 0x110), {menu}) : 0;
    Call(s, R64(R64(dialogs) + 0x3f8), {dialogs, 0, 0});
    // parameter block on the guest stack (ShowDialog copies it)
    const u64 sp = s.sp;
    const u64 p = (sp - 0x100) & ~0xFull;
    for (u64 o = 0; o < 0x60; o += 8)
        g_mem->Write<u64>(p + o, 0);
    W32(p + 0x00, 1);                                     // two buttons
    g_mem->Write<u64>(p + 0x08, okay);                    // OKAY
    g_mem->Write<u64>(p + 0x10, cancel);                  // CANCEL
    g_mem->Write<u64>(p + 0x28, g_quit_text[LanguageIndex()]); // message
    g_mem->Write<u64>(p + 0x48, hint);                    // button guide
    g_mem->Write<u64>(p + 0x50, g_main + kQuitCallback);  // callback(button)
    g_mem->Write<u64>(p + 0x58, 0x100000001ull);
    g_quit_pending = true;
    s.sp = p;
    Call(s, g_main + kShowDialog, {dialogs, p, owner});
    s.sp = sp;
    LOG_INFO("Menu", "Quit Game: asking to quit to the desktop");
}

// ---- hooks
// Refresh(this): rebuild the item lists, with QUIT GAME in a hidden main slot, moved to the bottom.
// Moves the entry of `slot` in a menu list to just after the entry of `after` (~0u: to the end).
void MoveEntry(u64 begin, u64 end, u32 slot, u32 after) {
    const u32 n = u32((end - begin) / kEntrySize);
    if (n > 16)
        return;
    u8 all[kEntrySize * 16];
    g_mem->ReadBlock(begin, all, n * kEntrySize);
    auto slot_at = [&](u32 k) { u32 v; std::memcpy(&v, all + k * kEntrySize, 4); return v; };
    u32 from = n;
    for (u32 k = 0; k < n; k++)
        if (slot_at(k) == slot)
            from = k;
    if (from == n)
        return;
    u8 e[kEntrySize];
    std::memcpy(e, all + from * kEntrySize, kEntrySize);
    std::memmove(all + from * kEntrySize, all + (from + 1) * kEntrySize, (n - from - 1) * kEntrySize);
    u32 to = n - 1;
    for (u32 k = 0; k + 1 < n; k++)
        if (slot_at(k) == after)
            to = k + 1;
    std::memmove(all + (to + 1) * kEntrySize, all + to * kEntrySize, (n - 1 - to) * kEntrySize);
    std::memcpy(all + to * kEntrySize, e, kEntrySize);
    g_mem->WriteBlock(begin, all, n * kEntrySize);
}

void Refresh(State& s) {
    const u64 menu = s.x[0];
    u32 borrow = ~0u, sys_borrow = ~0u, shown = 0, sys_shown = 0;
    bool tips_shown = false;
    if (IsMenu(menu)) {
        static const bool shield_test = std::getenv("MUA3_TEST_SHIELD_MENU") != nullptr;
        if (shield_test) // test aid: the pause menu is built as the S.H.I.E.L.D. point menu
            W32(menu + kRestricted, 0);
        for (u32 slot = 0; slot <= 10; slot++) {
            const u32 st = u32(Call(s, g_main + kGetItemState, {menu, slot}, &g_hooks[kHState]));
            if (slot <= kSlotSystem) {
                if (st != kHidden)
                    shown++;
                else if (slot >= 1 && slot <= 3)
                    borrow = slot; // HERO / INFINITY MISSION / LAB hidden here
            } else if (st != kHidden) {
                sys_shown++;
                tips_shown |= slot == kSlotTips;
            } else if (slot == kSlotSave || slot == kSlotLoad) {
                sys_borrow = slot; // the pause menu hides SAVE and LOAD
            }
        }
        if (shown >= kMainPositions || !g_pause_quit)
            borrow = ~0u; // no free position (S.H.I.E.L.D. point menu with everything unlocked)
        if (sys_shown >= kSysPositions) // the S.H.I.E.L.D. point menu: all five positions are taken; TIPS (still in
            sys_borrow = tips_shown ? kSlotTips : ~0u; // the pause menu) gives its place to GRAPHICS
        if (Call(s, g_main + kGetItemState, {menu, kSlotOptions}, &g_hooks[kHState]) == kHidden)
            sys_borrow = ~0u;
    }
    {
        std::lock_guard lk(g_mutex);
        if (borrow != ~0u)
            g_borrowed[menu] = borrow;
        else
            g_borrowed.erase(menu);
        if (sys_borrow != ~0u)
            g_sys_borrowed[menu] = sys_borrow;
        else
            g_sys_borrowed.erase(menu);
    }
    Original(s, kHRefresh);
    if (sys_borrow != ~0u) {
        // GRAPHICS goes right after OPTIONS, then the per-position enabled flags again
        const u64 begin = R64(menu + kSysListBegin), end = R64(menu + kSysListEnd);
        MoveEntry(begin, end, sys_borrow, kSlotOptions);
        const u32 n = u32((end - begin) / kEntrySize);
        for (u32 i = 0; i < kSysPositions; i++)
            W32(menu + kSysFlags + 4 * i, i < n ? R32(begin + i * kEntrySize + kEntryEnabled) : 0);
    }
    if (borrow == ~0u)
        return;
    // move the borrowed slot's entry to the end of the main list, then redo the per-position enabled flags
    const u64 begin = R64(menu + kMainListBegin), end = R64(menu + kMainListEnd);
    const u32 n = u32((end - begin) / kEntrySize);
    for (u32 k = 0; k + 1 < n; k++) {
        if (R32(begin + k * kEntrySize) != borrow)
            continue;
        u8 e[kEntrySize], rest[kEntrySize * 16];
        const u32 tail = std::min(n - k - 1, 16u);
        g_mem->ReadBlock(begin + k * kEntrySize, e, kEntrySize);
        g_mem->ReadBlock(begin + (k + 1) * kEntrySize, rest, tail * kEntrySize);
        g_mem->WriteBlock(begin + k * kEntrySize, rest, tail * kEntrySize);
        g_mem->WriteBlock(begin + (k + tail) * kEntrySize, e, kEntrySize);
        break;
    }
    for (u32 i = 0; i < kMainPositions; i++)
        W32(menu + kMainFlags + 4 * i, i < n ? R32(begin + i * kEntrySize + kEntryEnabled) : 0);
}

// GetItemState(this, slot): the borrowed slot is shown.
void GetItemState(State& s) {
    if (Borrowed(s.x[0]) == u32(s.x[1]) || SysBorrowed(s.x[0]) == u32(s.x[1]))
        return Return(s, 0);
    Original(s, kHState);
}

// SetItemAppearance(this, slot, state): the borrowed slot wears the QUIT GAME label.
void SetItemAppearance(State& s) {
    const u32 slot = u32(s.x[1]);
    if (slot > 10 || (Borrowed(s.x[0]) != slot && SysBorrowed(s.x[0]) != slot))
        return Original(s, kHAppearance);
    const u64 off = g_main + kTexOff + 4 * slot, grey = g_main + kTexGrey + 4 * slot, on = g_main + kTexOn + 4 * slot;
    const u32 saved[3] = {R32(off), R32(grey), R32(on)};
    if (SysBorrowed(s.x[0]) == slot) { // GRAPHICS (a SYSTEM submenu slot: the submenu's label pane already)
        W32(off, kTexHeroGrey);
        W32(grey, ~0u);
        W32(on, kTexInfinityGrey);
        Original(s, kHAppearance);
        W32(off, saved[0]);
        W32(grey, saved[1]);
        W32(on, saved[2]);
        return;
    }
    W32(off, kTexQuitGame);
    W32(grey, ~0u);
    W32(on, kTexQuitGameOn);
    t_system_style = slot; // GText_quit_game is drawn for the SYSTEM submenu's (non-italic, wider) label pane
    Original(s, kHAppearance);
    t_system_style = ~0u;
    W32(off, saved[0]);
    W32(grey, saved[1]);
    W32(on, saved[2]);
}

// IsSystemItem(this, slot): while its label is set, the borrowed slot uses the submenu's label pane.
void IsSystemItem(State& s) {
    if (t_system_style == u32(s.x[1]) && Borrowed(s.x[0]) == t_system_style)
        return Return(s, 1);
    Original(s, kHSystem);
}

// ExecuteItem(this, slot): the borrowed slot asks to quit to the desktop.
void ExecuteItem(State& s) {
    const u64 menu = s.x[0];
    if (SysBorrowed(menu) == u32(s.x[1])) { // GRAPHICS: OPTIONS, shown as the GRAPHICS page
        LOG_INFO("Menu", "GRAPHICS chosen in the {} menu", R32(menu + kRestricted) == 1 ? "pause" : "S.H.I.E.L.D. point");
        graphics::OpenNext();
        s.x[1] = kSlotOptions;
        return Original(s, kHExecute);
    }
    if (Borrowed(menu) != u32(s.x[1])) {
        g_quit_pending = false; // e.g. the SYSTEM submenu's QUIT GAME: its dialog returns to the title screen
        return Original(s, kHExecute);
    }
    ShowQuitDialog(s, menu);
    Return(s, 0);
}

// The QUIT GAME dialog's callback(button): after our dialog, OKAY closes the game.
void QuitCallback(State& s) {
    if (!g_quit_pending.exchange(false))
        return Original(s, kHQuit);
    if (u32(s.x[0]) == 0) {
        LOG_INFO("Menu", "Quit Game confirmed");
        core::Sys().RequestExit();
    }
    Return(s, 0);
}

// The title menu's "Nintendo eShop" item (no use on a PC) is QUIT GAME: it asks to quit to the desktop. Other
// callers get the game's own function.
void StartShop(State& s) {
    if (s.x[30] != g_main + kTitleShopRet)
        return Original(s, kHShop);
    ShowQuitDialog(s, 0);
    Return(s, 0);
}
void ShopOrNotice(State& s) {
    if (s.x[30] != g_main + kTitleNoticeRet)
        return Original(s, kHNotice);
    ShowQuitDialog(s, 0);
    Return(s, 0);
}

// ---- the title menu's SYSTEM submenu ends with GRAPHICS: a hidden main-list slot is shown there, last, with the
// GRAPHICS label, and opens the OPTIONS screen as the GRAPHICS page (menu_graphics.cpp).
u32 TitleBorrowed(u64 menu) {
    std::lock_guard lk(g_mutex);
    auto it = g_title_borrowed.find(menu);
    return it == g_title_borrowed.end() ? ~0u : it->second;
}
bool IsTitle(u64 obj) { return obj && R64(obj) == g_main + kTitleVtable; }

void TitleRefresh(State& s) {
    const u64 menu = s.x[0];
    u32 borrow = ~0u;
    if (IsTitle(menu))
        for (u32 slot : kTitleBorrowable)
            if (Call(s, g_main + kTitleState, {menu, slot}, &g_hooks[kHTState]) == kHidden) {
                borrow = slot;
                break;
            }
    {
        std::lock_guard lk(g_mutex);
        if (borrow != ~0u)
            g_title_borrowed[menu] = borrow;
        else
            g_title_borrowed.erase(menu);
    }
    Original(s, kHTRefresh);
    if (borrow == ~0u)
        return;
    // the borrowed slot (listed first, in slot order) goes last; positions are by list index: the game's 5, then the
    // spare panes
    const u64 begin = R64(menu + 0xe0), end = R64(menu + 0xe8);
    const u32 n = u32((end - begin) / kEntrySize);
    for (u32 k = 0; k + 1 < n; k++) {
        if (R32(begin + k * kEntrySize) != borrow)
            continue;
        u8 e[kEntrySize], rest[kEntrySize * 16];
        const u32 tail = std::min(n - k - 1, 16u);
        g_mem->ReadBlock(begin + k * kEntrySize, e, kEntrySize);
        g_mem->ReadBlock(begin + (k + 1) * kEntrySize, rest, tail * kEntrySize);
        g_mem->WriteBlock(begin + k * kEntrySize, rest, tail * kEntrySize);
        g_mem->WriteBlock(begin + (k + tail) * kEntrySize, e, kEntrySize);
        break;
    }
    for (u32 i = 0; i < n; i++) {
        const bool spare = i >= kTitleSystemPositions;
        W32(begin + i * kEntrySize + 4, spare ? kCursorSpare : R32(g_main + kTitleSysCursor + 4 * i));
        W32(begin + i * kEntrySize + 8, spare ? kTextSpare : R32(g_main + kTitleSysText + 4 * i));
    }
}

void TitleState(State& s) {
    if (TitleBorrowed(s.x[0]) == u32(s.x[1]))
        return Return(s, 0);
    Original(s, kHTState);
}
void TitleIsSystem(State& s) {
    if (TitleBorrowed(s.x[0]) == u32(s.x[1]))
        return Return(s, 1);
    Original(s, kHTSystem);
}
// SetItemAppearance(this, slot, state): the borrowed slot wears the GRAPHICS label.
void TitleAppearance(State& s) {
    const u32 slot = u32(s.x[1]);
    if (slot >= 16 || TitleBorrowed(s.x[0]) != slot)
        return Original(s, kHTAppearance);
    const u64 off = g_main + kTitleTexOff + 4 * slot, on = g_main + kTitleTexOn + 4 * slot;
    const u32 saved[2] = {R32(off), R32(on)};
    W32(off, kTexMainMenu);
    W32(on, kTexMainMenuOn);
    Original(s, kHTAppearance);
    W32(off, saved[0]);
    W32(on, saved[1]);
}
// Execute(this, slot): GRAPHICS opens the OPTIONS screen as the GRAPHICS page.
void TitleExecute(State& s) {
    if (TitleBorrowed(s.x[0]) != u32(s.x[1]))
        return Original(s, kHTExecute);
    LOG_INFO("Menu", "GRAPHICS chosen in the title menu");
    graphics::OpenNext();
    s.x[1] = kTitleOptions;
    Original(s, kHTExecute);
}

// Every frame: the spare position panes go one step below the SYSTEM submenu's last position (the step from its
// 4th to its 5th), following the submenu's animations.
float F32(u64 a) { return std::bit_cast<float>(R32(a)); }
void PlaceSpare(State& s, u64 layout, u32 sub4, u32 sub5, u32 spare) {
    const u64 a = Call(s, g_main + kPane, {layout, sub4}), b = Call(s, g_main + kPane, {layout, sub5}),
              p = Call(s, g_main + kPane, {layout, spare});
    if (!a || !b || !p)
        return;
    auto world = [&](u64 pane, int k) {
        const u64 m = R64(pane + (R64(pane + kPaneParent) ? kPaneWorld : kPaneLocal));
        return m ? F32(m + 0x30 + 4 * k) : 0.0f;
    };
    const u64 parent = R64(p + kPaneParent), local = R64(p + kPaneLocal);
    if (!local)
        return;
    for (int k = 0; k < 2; k++) {
        const float want = 2 * world(b, k) - world(a, k);
        W32(local + 0x30 + 4 * k, std::bit_cast<u32>(want - (parent ? world(parent, k) : 0.0f)));
    }
    W32(p + kPaneFlags, R32(p + kPaneFlags) | kPaneDirty);
    // the items take their colour (alpha: fading in and out with their list) from their position pane; the spare
    // pane belongs to the main list, which fades out while the submenu shows
    W32(p + kPaneColor, R32(b + kPaneColor));
}
void TitleUpdate(State& s) {
    const u64 menu = s.x[0];
    if (IsTitle(menu) && TitleBorrowed(menu) != ~0u) {
        const u64 anims = R64(menu + 0xa0), layout = anims ? R64(anims + 8) : 0;
        if (layout) {
            PlaceSpare(s, layout, kCursorSub4, kCursorSub5, kCursorSpare);
            PlaceSpare(s, layout, kTextSub4, kTextSub5, kTextSpare);
        }
    }
    Original(s, kHTUpdate);
}

// ---- the title menu's QUIT GAME label: GText_DLC / GText_DLC_on (the "Nintendo eShop" label) of every
// language's 10_title_menu_parts pack are replaced, as the game reads them, by labels drawn in the same style.
struct Overlay {
    std::string file; // file name in the RomFS
    u64 off;          // in the file
    std::vector<u8> bytes;
};
std::vector<Overlay> g_overlays; // written once by Install, before the game runs

struct QuitLabel {
    const char* lang;
    const wchar_t* text;
    const wchar_t* face;
    bool cjk;
};
const QuitLabel kQuitLabels[] = {
    {"eng", L"QUIT GAME", L"Segoe UI Black", false},
    {"fra", L"QUITTER LE JEU", L"Segoe UI Black", false},
    {"ger", L"SPIEL BEENDEN", L"Segoe UI Black", false},
    {"ita", L"ESCI DAL GIOCO", L"Segoe UI Black", false},
    {"esp", L"SALIR DEL JUEGO", L"Segoe UI Black", false},
    {"jpn", L"ゲーム終了", L"Yu Gothic", true},
    {"kor", L"게임 종료", L"Malgun Gothic", true},
    {"chn", L"結束遊戲", L"Microsoft JhengHei", true},
    {"smp", L"退出游戏", L"Microsoft YaHei", true},
};

// GRAPHICS: the game's own word for its graphics option (cut from the Switch version), in every language
const QuitLabel kGraphicsLabels[] = {
    {"eng", L"GRAPHICS", L"Segoe UI Black", false},
    {"fra", L"AFFICHAGE", L"Segoe UI Black", false},
    {"ger", L"GRAFIK", L"Segoe UI Black", false},
    {"ita", L"GRAFICA", L"Segoe UI Black", false},
    {"esp", L"GR\u00c1FICOS", L"Segoe UI Black", false},
    {"jpn", L"\u30b0\u30e9\u30d5\u30a3\u30c3\u30af", L"Yu Gothic", true},
    {"kor", L"\uadf8\ub798\ud53d", L"Malgun Gothic", true},
    {"chn", L"\u756b\u9762", L"Microsoft JhengHei", true},
    {"smp", L"\u753b\u9762", L"Microsoft YaHei", true},
};

// Draws `labels` over two textures (off, on) of every language's pack of a layout, as the game reads them.
// `render(label, selected, w, h)` draws one in the texture's size.
template <typename Render>
void BuildPackLabels(const char* layout, const QuitLabel* labels, size_t count, const std::string& off,
                     const std::string& on, Render render) {
    const fs::path dir = core::Sys().Cfg().game_dir / "romfs" / "UI" / "screenlayout" / layout;
    for (const QuitLabel& l : std::span(labels, count)) {
        const std::string name = std::string(layout) + "_" + l.lang + ".kslt";
        const auto d = common::ReadFile(dir / name);
        auto u32at = [&](size_t o) { u32 v = 0; if (o + 4 <= d.size()) std::memcpy(&v, &d[o], 4); return v; };
        if (d.size() < 0x40 || std::memcmp(d.data(), "TLSK3100", 8) != 0)
            continue;
        const u32 n = u32at(8), params = u32at(0x10);
        const size_t offs = 0x40 + size_t(params);
        size_t p = offs + 0x14 * size_t(n);
        for (u32 i = 0; i < n && p < d.size(); i++) {
            const size_t e = std::find(d.begin() + p, d.end(), u8(0)) - d.begin();
            const std::string tex(d.begin() + p, d.begin() + e);
            p = e + 1;
            if (tex != off && tex != on)
                continue;
            const size_t at = u32at(offs + 0x14 * i);
            if (at + 0x48 > d.size())
                continue;
            u16 w, h;
            std::memcpy(&w, &d[at + 4], 2);
            std::memcpy(&h, &d[at + 6], 2);
            const u32 size = u32at(at + 0x1c);
            auto px = render(l, tex == on, w, h);
            if (px.size() != size) {
                LOG_WARN("Menu", "{} label for {} not drawn ({} {}x{})", off, l.lang, tex, w, h);
                continue;
            }
            g_overlays.push_back({name, at + 0x48, std::move(px)});
        }
    }
}

// The title menu's labels (10_title_menu_parts): heavy italic, like the game's
void BuildTitleLabels(const QuitLabel* labels, size_t count, const char* texture) {
    BuildPackLabels("10_title_menu_parts", labels, count, texture, std::string(texture) + "_on",
                    [](const QuitLabel& l, bool sel, int w, int h) {
                        // the game's label metrics (TEAM / TEAM_on): capitals 24 px of 44 from y 7, 34 px of 60 from y 12
                        const LabelStyle st{w, h, sel ? h * 34 / 60 : h * 24 / 44, sel ? 14 : 9,
                                            sel ? h * 12 / 60 : h * 7 / 44, sel};
                        return RenderLabel(st, l.text, l.face, l.cjk);
                    });
}

} // namespace

void PatchRomfsRead(const fs::path& host, u64 off, u8* dst, size_t n) {
    if (g_overlays.empty())
        return;
    const std::string name = host.filename().string();
    for (const Overlay& o : g_overlays) {
        if (o.file != name || off >= o.off + o.bytes.size() || o.off >= off + n)
            continue;
        const u64 a = std::max(off, o.off), b = std::min(off + n, o.off + o.bytes.size());
        std::memcpy(dst + (a - off), o.bytes.data() + (a - o.off), size_t(b - a));
    }
}

void Install(core::Memory& mem, kernel::Kernel& kernel, u64 main_base) {
    if (const char* e = std::getenv("MUA3_MENU_MODS"); e && e[0] == '0')
        return;
    // the build these offsets come from: the first words of every function and table used
    static constexpr std::pair<u64, u32> kExpect[] = {
        {kRefresh, 0xf81e0ff3},     {kGetItemState, 0xb9417c08}, {kSetItemAppearance, 0xf81b0ff9},
        {kExecuteItem, 0xd10283ff}, {kIsSystemItem, 0x51001828}, {kQuitCallback, 0x34000040}, {kShowDialog, 0xf81d0ff5},
        {kGetString, 0x2a0103e8},   {kTexOff, 13},               {kTexGrey, ~0u},
        {kTexOn, 14},               {kStartShop, 0xf81e0ff3},   {kShopOrNotice, 0xd101c3ff},
        {kTitleRefresh, 0xd10203ff}, {kTitleState, 0xa9bc5ff8}, {kTitleAppearance, 0xfc1c0fe8},
        {kTitleIsSystem, 0x51002c28}, {kTitleExecute, 0xd10243ff}, {kTitleUpdate, 0xd102c3ff},
        {kTitleTexOff + 4 * kTitleOptions, 36}, {kTitleSysCursor, 42}, {kTitleSysText, 63},
    };
    for (auto [off, word] : kExpect)
        if (!mem.IsValid(main_base + off, 4) || mem.Read<u32>(main_base + off) != word) {
            LOG_WARN("Menu", "unknown game build (main+{:#x}): menu extensions off", off);
            return;
        }
    g_mem = &mem;
    g_main = main_base;
    // our strings
    constexpr u64 kTextSize = 0x4000;
    g_text = mem.FindFree(core::layout::kKernelMapBase, 0x1000000000ull, kTextSize);
    mem.Map(g_text, kTextSize, core::MS_Normal, core::MP_RW);
    u64 at = g_text;
    for (size_t i = 0; i < std::size(kQuitToDesktop); i++) {
        const std::u16string t = kQuitToDesktop[i].text;
        g_quit_text[i] = at;
        mem.WriteBlock(at, t.c_str(), (t.size() + 1) * 2);
        at = (at + (t.size() + 1) * 2 + 15) & ~15ull;
    }
    const std::pair<u64, void (*)(State&)> hooks[kHookCount] = {
        {kRefresh, Refresh},         {kGetItemState, GetItemState}, {kSetItemAppearance, SetItemAppearance},
        {kIsSystemItem, IsSystemItem}, {kExecuteItem, ExecuteItem}, {kQuitCallback, QuitCallback},
        {kStartShop, StartShop},     {kShopOrNotice, ShopOrNotice}, {kTitleRefresh, TitleRefresh},
        {kTitleState, TitleState},   {kTitleAppearance, TitleAppearance}, {kTitleIsSystem, TitleIsSystem},
        {kTitleExecute, TitleExecute}, {kTitleUpdate, TitleUpdate},
    };
    // QUIT GAME in the pause menu too (MUA3_PAUSE_QUIT=1): the title screen's is the default
    const char* pause = std::getenv("MUA3_PAUSE_QUIT");
    const bool pause_quit = pause && pause[0] == '1';
    g_pause_quit = pause_quit;
    for (int i = 0; i < kHookCount; i++) {
        g_hooks[i].pc = main_base + hooks[i].first;
        g_hooks[i].host = hooks[i].second;
        g_hooks[i].direct = i == kHExecute || i == kHShop || i == kHNotice; // also called with BL
        kernel.Cpus().Override(g_hooks[i]);
    }
    BuildTitleLabels(kQuitLabels, std::size(kQuitLabels), "GText_DLC");
    BuildTitleLabels(kGraphicsLabels, std::size(kGraphicsLabels), "GText_MAIN_MENU");
    // the pause menu's GRAPHICS: the SYSTEM submenu's label style, stretched over the two unused grey textures
    BuildPackLabels("01_savepoint_parts", kGraphicsLabels, std::size(kGraphicsLabels), "GText_HERO_gray_out",
                    "GText_INFINITY_MISSION_gray_out", [](const QuitLabel& l, bool sel, int w, int h) {
                        return RenderSystemLabel(l.text, l.face == std::wstring(L"Segoe UI Black") ? L"Segoe UI Light" : l.face,
                                                 l.cjk, sel, w, h);
                    });
    graphics::Install(mem, kernel, main_base);
    if (const char* e = std::getenv("MUA3_GRAPHICS_TEST"); e && e[0] == '1')
        graphics::OpenNext(); // test aid: the first OPTIONS screen opened shows the GRAPHICS page
    LOG_INFO("Menu", "menu extensions installed: QUIT GAME and GRAPHICS on the title screen, GRAPHICS in the pause "
             "menu ({} label textures){}", g_overlays.size(), pause_quit ? ", QUIT GAME in the pause menu" : "");
}

} // namespace hle::menu
