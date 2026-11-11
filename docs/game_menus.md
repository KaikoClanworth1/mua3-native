# The game's menus, and how to add options to them

Research notes for extending MUA3's own menus with PC-style options. Main module offsets are for 4.0.1 (build id
e853c44fdf18b88f, main loaded at 0x8004000; "main+X" = offset into the module image,
`build/recomp_work/nso/main.bin`). Implementation: `src/core/hle/menu_mods.cpp`.

## What the menus are made of

### Data (RomFS `UI/`)

| What | Where | Format |
|---|---|---|
| Screen layouts | `UI/screenlayout/<name>/<name>.kscl` (+ `.kslt` texture pack, `_<lang>.kslt` per-language textures) | KTGL layout ("LCSK4500" / "TLSK3100") |
| Pane, group, animation, texture names | `<name>_pane_name.bin`, `_pane_group_name.bin`, `_anime.bin`, `_texture_name.bin` | u32 0, u32 count, u32 0, offsets, names. Code refers to panes / animations / textures by **index** in these lists, never by name |
| Text | `UI/string/mes_<table>_<lang>.bin` | UTF-16LE string table (u32 count + offsets) |
| Text labels | `UI/string/hash_table/hash_table_<table>.bin` | (entry id, hash, hash) triples. Entry id = string index + 1. Code looks text up by **hash**: `GetString(table, hash)` = main+0x1d6290, system table = `*[*[main+0x2187ce8]]` |

Languages: eng, fra, ger, ita, esp, jpn, kor, chn (traditional), smp (simplified).

Menu item labels are not drawn from the string tables: they are pre-rendered images (`GText_*` textures) in every
language's texture pack, so a label can only be one the game already has, or a new texture added to all nine
packs.

### The main menu (pause menu and S.H.I.E.L.D. point menu)

![layout] Layout `01_savepoint` (cosmic background, the big "3", `Null_01_pos`..`Null_06_pos` for 6 main items,
`Null_system_01_pos`..`_05_pos` for the SYSTEM submenu, cursor animations `cursor_pos_01..06` /
`system_cursor_pos_01..06`). Each item is an instance of `01_savepoint_parts` (panes `GText_dummy` /
`GText_dummy_on` for main items, `system_GText_dummy` / `_on` for submenu items; animations `menu_ON/OFF`,
`system_menu_ON/OFF`).

Labels available in `01_savepoint_parts` (texture index = position in its alphabetical texture list):

| idx | texture | idx | texture |
|---|---|---|---|
| 0, 1 | GText_COOP, _on | 13, 14 | GText_TEAM, _on |
| 2, 3, 4 | GText_HERO, _gray_out, _on | 15, 16 | GText_load, _on |
| 5, 6, 7 | GText_INFINITY_MISSION, _gray_out, _on | 17, 18 | GText_option, _on |
| 8, 9, 10 | GText_LAB, _gray_out, _on | 19, 20 | GText_quit_game, _on |
| 11, 12 | GText_SYSTEM, _on | 21, 22 / 23, 24 | GText_save, _on / GText_tutorial, _on |

One C++ class drives both menus: vtable main+0x2109f88 (no RTTI in this build). It has **11 fixed item slots**:

| slot | item | list | ExecuteItem action |
|---|---|---|---|
| 0 | TEAM | main | screen 4 (vfunc +0x3d0) |
| 1 | HERO | main | main+0x654750, screen 0x3d |
| 2 | INFINITY MISSION | main | main+0x638600 |
| 3 | LAB | main | (main+0x6e0940) |
| 4 | CO-OP | main | main+0x6e2900 |
| 5 | SYSTEM | main | opens the submenu (this+0xc0 = 1) |
| 6 | SAVE | system | main+0x6e2a10 |
| 7 | LOAD | system | main+0x6e2b20 |
| 8 | OPTIONS | system | main+0x6da8d0, screen 0x5f |
| 9 | TUTORIAL | system | main+0x6da9f0, screen 0x63 |
| 10 | QUIT GAME | system | yes/no dialog, OKAY returns to the title screen |

Per-slot data: label texture tables (u32[11] each, -1 = none): off main+0x1a3cb20, grey main+0x1a3cb4c, on
main+0x1a3cb78. Position panes by list index: main+0x1a3caf4 (6 entries: panes 70..75 = `Null_01_pos`..) and
main+0x1a3cb0c (5: `Null_system_01_pos`..).

Object layout (this = menu object):

| offset | contents |
|---|---|
| +0xa0 | layout (01_savepoint) helper: animations by index (main+0x5e4d00) |
| +0xb0 | cursor/list controller: vfunc +0x28 init(count), +0x58 GetCursor, +0x60 SetCursor, +0x180 SetEnabled(flags, n) |
| +0xbc | input state |
| +0xc0 | 1 = inside the SYSTEM submenu |
| +0xc8 / +0xd0 | main list: std::vector of 20-byte entries {u32 slot, ..., u32 enabled @+0x10} |
| +0xe0 / +0xe8 | system list (same entries) |
| +0xf8 + 8*slot | the slot's `01_savepoint_parts` instance (11 pointers, up to +0x150, where other members begin) |
| +0x178 | state machine step (vfunc +0x3e8, main+0x6e04f0) |
| +0x17c | 1 = restricted context (table main+0x1a63e50: slots 1-3 and 6-7 hidden) |
| +0x180 / +0x198 | enabled flag per main (6) / system (5) list position |

Virtual functions that matter:

| vfunc | main+ | what |
|---|---|---|
| +0x448 | 0x6e2e90 | Refresh: BuildItemLists (0x6e3000) + copy enabled flags to +0x180/+0x198 |
| +0x450 | 0x728970 | slot at a cursor position |
| +0x460 | 0x6e3150 | GetItemState(slot): 0 shown, 1 grey, 2 hidden |
| +0x480 | 0x6e31f0 | SetItemAppearance(slot, state): label textures from the tables |
| +0x490 | 0x7e33f0 | IsSystemItem(slot): 6 <= slot < 11 |
| +0x498 | 0x7e3400 | slot count: 11 |
| +0x410 | 0x6e3620 | A pressed: decide animation, first-use tutorials |
| +0x3e8 | 0x6e04f0 | state machine: after the decide animation calls ExecuteItem(slot) (main+0x6e0890, direct BL) |

BuildItemLists: for slot in 0..count-1: state = GetItemState(slot); if not hidden, append {slot, enabled} to the
main or system list (IsSystemItem); SetItemAppearance(slot, state). List order = slot order; list position i is
placed at pane `Null_0<i+1>_pos`.

### Dialogs

`ShowDialog(manager, params, owner)` = main+0x628940; manager = `*(*[*[main+0x2187dc0]] + 0x30)`; call its vfunc
+0x3f8(0, 0) first; owner = menu vfunc +0x110. The 0x60-byte parameter block is copied:

| offset | value |
|---|---|
| +0x00 | u32 1 = two buttons |
| +0x08, +0x10 | button labels (char16_t*): OKAY (system hash 0x3a471d), CANCEL (0x0cb958) |
| +0x28 | message (char16_t*, kept by pointer: must stay valid) |
| +0x48 | button guide (0x3303db, "A SELECT") |
| +0x50 | callback(u32 button), 0 = first button |
| +0x58 | u64 0x100000001 |

Text markup in messages: `\r\n` line breaks, `^00~A_BTN~` button icons, `^01~CAUTION~` styled words.

## How to add an option

The class cannot get a 12th slot (the parts pointers run into other members), so a new main-menu option
**borrows a slot the game hides in the current context** and is moved to the bottom of the list. In the pause
menu HERO, INFINITY MISSION and LAB (slots 1-3) are hidden, and only 3 of the 6 positions are used.

The game functions are replaced by host functions through `cpu::FunctionOverride` (`core/cpu/cpu.h`):
indirect calls and tail jumps to the function reach the host; with `direct`, also direct BL calls (the
function's own native code is disabled with its enable flag and re-enabled around calls to the original). No
guest code is patched and the recompiled library does not change.

The hooks (menu_mods.cpp):

1. **Refresh** (+0x448): picks the borrowed slot (a hidden slot 1-3, only when fewer than 6 items are shown), runs
   the game's Refresh, then moves the borrowed entry to the end of the main list and redoes the enabled flags.
2. **GetItemState** (+0x460): the borrowed slot is shown.
3. **SetItemAppearance** (+0x480): while the game sets the borrowed slot's label, its table entries hold the new
   label's texture indices.
4. **ExecuteItem** (direct): the borrowed slot runs the new action instead of the original one.
5. Optional: the action's own callbacks (the Quit dialog's callback, main+0x6e2c30, is reused with a flag).

To add another option: give it a label (a texture index above, or new `GText_*` textures in all nine
`01_savepoint_parts_<lang>.kslt` packs), a context where a slot is free, and an action in the ExecuteItem hook
(guest calls go through `Call()`, which runs guest code with the guest calling convention from a host function).
SYSTEM submenu options work the same way with slots 6-10 (in the pause menu SAVE and LOAD are hidden).

## The title-screen main menu

The title menu (layout `10_title_menu_02`: CONTINUE, CHAPTER SELECT, NEW GAME, TEAM, CO-OP, SYSTEM, Nintendo
eShop) is a different class with fixed items. Its labels are textures in `10_title_menu_parts_<lang>.kslt`
(TLSK3100 packs, 46 textures: `GText_*` 456x44, `GText_*_on` 648x60 when selected, BC3 in linear block order;
header: count at +8, params at 0x40 (count*0x38), then count*0x14 offset entries, then NUL-terminated names; each
texture has a 0x48-byte header with u16 width/height at +4/+6 and the data size at +0x1c).

The last item, **Nintendo eShop** (`GText_DLC` / `GText_DLC_on`), has nothing to open on PC, so it becomes the
new option:

- **Action**: the title menu's item code calls StartShop (main+0x840850, which starts the eShop applet on a
  worker thread) from main+0x720f34, or, without a usable account, main+0x6e57c0 (eShop or a notice) from
  main+0x720ffc. Both get direct overrides that, for those return addresses only, open the quit dialog (owner 0)
  and return 0 (the menu stays as it is); every other caller runs the game's function.
- **Label**: the new label is drawn at start-up with Windows fonts in the game's lettering style (menu_label.cpp:
  heavy italic capitals, slanted a little further, soft drop shadow; white, or the yellow gradient for `_on`;
  CJK labels use Yu Gothic / Malgun Gothic / JhengHei / YaHei) and BC3-encoded. RomFS reads of the nine packs are
  patched with it (`hle::menu::PatchRomfsRead`, called from `fs.cpp`), so the game loads it like its own texture.
  Only the in-memory read is changed; the files on disk stay as they are.

To give the title menu another option the same way: pick an item to take over, hook its handler, and overlay its
two label textures.

`MUA3_TRACE_NATIVE=main+0x...` (native_trace.cpp) logs x0-x3 and the caller of a native function, to find a
handler: trace the candidates and press the item.

## Quit Game

The title-screen main menu ends with QUIT GAME, in place of Nintendo eShop. It opens the game's dialog with
"Do you want to quit the game and return to the desktop?" (translated for all nine text languages, using only
characters the game's fonts already contain); OKAY closes the game cleanly, CANCEL returns to the menu.

With `MUA3_PAUSE_QUIT=1` the pause menu also gets a QUIT GAME (a borrowed slot, as above; the game's own label).
The SYSTEM submenu's QUIT GAME still returns to the title screen. `MUA3_MENU_MODS=0` turns all extensions off.

## The OPTIONS screen

Screen 0x5f (opened by main+0x6da8d0 and the menu's vfunc +0x3d0), class vtable main+0x2121e40. Layouts
`09_option_BG` (title, `Null_list_01..08_pos` for 8 visible rows, scroll bar, `option_info` description),
`09_option_tub_menu` (5 tabs: `Window_tab_0N`, `Window_tab_ON_0N`, `tab_name_0N` = panes 1+3N..3+3N; L/R icons
16-19; animations `tab_0N_ON/OFF` = 2+2N / 3+2N) and `09_option_parts` (one instance per visible row).

Everything a tab shows is data, per tab (INTERFACE, CAMERA, CONTROLS, SOUND, OTHER = 0-4):

| what | where |
|---|---|
| row count | u16[5] at main+0x1a634b2 (6, 6, 3, 4, 2) |
| row types | u32 arrays: tab 0 main+0x1a634bc, 1 +0x1a634d4, 2 +0x1a634ec, 3 main+0x19f4530, 4 main+0x19f78b8 (shared constant data: do not write). 0 button (A opens a screen), 1 two choices, 2 arrows, 3 gauge |
| rows | GOT entries main+0x218bd30 + 8*tab: `{u32 label hash, u32 description hash, u64 choices}`, choices = u32 array of label hashes (system string table) |
| values | GOT entries (tab 0 +0x218bd10, 1 +0x218bd18, 2 +0x218bd20, 3 +0x218bd28, 4 +0x218bd08): one byte per row (type 1: 0 = first choice; type 2: the choice index) |

| function | main+ | what |
|---|---|---|
| vfunc +0x3f8 | 0x7c5050 | open: the game's settings into the value bytes, tab 0, rows laid out (0x7c5900) |
| vfunc +0x400 | 0x7c6050 | close: 0x7c60a0 copies the value bytes back into the game's settings |
| vfunc +0x1b8 / +0x1c0 | 0x7c8480 / 0x7c9400 | left / right on a row: steps its byte (arrows wrap at ChoiceCount) |
| vfunc +0x1e8 / +0x1f0 | 0x7c81e0 / 0x7c8320 | L / R: tab |
| vfunc +0x228 | 0x7c7c10 | A on a row (button rows: tab 2 rows 1-2, tab 4 row 1) |
| vfunc +0x230 | 0x7c7e40 | B: close |
| | 0x7c91f0 | ChoiceCount(this, row) of an arrow row (tab 0 rows 1-4 from a table, tab 3 gauges 10, tab 1 row 5 the players) |

Special cases by (tab, row): tab 0 row 1 is the language selector (0x7c7b00); tab 1 row 1 is greyed out when row
0's value is 1 (0x7c8e90); tabs 1 and 4 refresh the next row after a change.

## GRAPHICS

The title screen's SYSTEM submenu (SAVE, LOAD, OPTIONS, TIPS, GALLERY; title menu class vtable main+0x210ee80,
16 slots, 11-15 = the submenu) ends with **GRAPHICS**:

- **Item**: a main-list slot hidden on the title screen (MENU SELECT, LAB, HERO or INFINITY) is shown in the
  submenu (GetItemState vfunc +0x460 and IsSystemItem +0x490 hooked) and moved to its end (Refresh +0x448). The
  submenu has 5 position panes (`Null_cursor_sub_0N_pos` / `Null_GText_sub_0N_pos`, tables main+0x1a3d27c /
  0x1a3d23c by list index); the 6th entry uses the unused 12th main-list positions (`Null_cursor_12_pos`,
  `Null_GText_12_pos`), which the per-frame vfunc +0x3f0 (main+0x71f760) moves one step below the 5th (pane object:
  +0xd0 parent, +0xe0 world / +0xe8 local transform with the translation at +0x30, +0x104 flags, 0x10 = dirty).
- **Label**: the unused `GText_MAIN_MENU(_on)` textures, drawn as the game's own word for graphics in each
  language (GRAPHICS, AFFICHAGE, GRAFIK, ...); SetItemAppearance (+0x480) uses them for the slot.
- **Page**: Execute (+0x4a8) opens OPTIONS the game's way, flagged to show the GRAPHICS page
  (`menu_graphics.cpp`): tab 2's row count, row types, row table and value bytes are replaced while it is open, the
  tab is named GRAPHICS, the other tabs and L/R are hidden, L/R do nothing, A steps the row to its next value (wrapping), our text comes from a GetString
  hook (hashes 0xFE00xxxx). Closing puts the tab back and skips the store, so the game's own settings are untouched.
- **Settings**: DISPLAY MODE, RESOLUTION, RENDER SCALE, FRAME RATE, FPS LIMIT, VSYNC, SHOW FPS, ANTI-ALIASING
  (mua3.ini [video], the launcher's choices). Every change goes through `core::System::UpdateConfig`: the running
  config (FPS limit, show FPS, VSync, window mode and size: the main loop applies them; anti-aliasing, OFF / FXAA:
  the renderer reads it at every present) and mua3.ini (also render scale and frame rate, which apply after a
  restart). The page shows the running config, except RENDER SCALE and FRAME RATE, which show the value saved in
  mua3.ini for the next start; their info line names the value running now.

`MUA3_GRAPHICS_TEST=1` (test aid) opens the first OPTIONS screen as the GRAPHICS page.
