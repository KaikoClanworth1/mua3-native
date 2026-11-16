# Mods

The native port loads MUA3 mods made for the Switch version: mods that replace or add game files (LayeredFS /
"romfs" mods: retextures, edited data tables, UI and audio files). Drop a mod's folder into `mods\` and start the game.

## Installing a mod

1. Open the `mods` folder next to `mua3.exe` (the game makes it on first start).
2. Put the mod's folder in it, one folder per mod. The folder name is the mod's name. Any of these layouts works
   unchanged, so a mod made for an emulator or a modded Switch can be dropped in as it comes:

   | Layout inside `mods\<Mod name>\` | Where it comes from |
   | --- | --- |
   | `romfs\...` | Yuzu / Ryujinx / Sudachi per-mod folders (`load\010060700AC50000\<Mod name>\romfs`) |
   | `010060700AC50000\romfs\...` | most mod downloads |
   | `atmosphere\contents\010060700AC50000\romfs\...` | a modded Switch's SD card |
   | `contents\010060700AC50000\romfs\...` | |
   | `DATA\`, `UI\`, `ACTION\`, ... directly | the game's own folders |

   A mod zip that extracts into one extra folder level is found too. A whole emulator folder works as well:
   `mods\010060700AC50000\<Mod A>\romfs`, `mods\010060700AC50000\<Mod B>\romfs`, ... become mods A, B, ...
3. Start the game. Mods are read once at start: after adding, removing or reordering mods, restart the game.

`mods\mods.log` lists every mod after each start: what it replaces, what it adds, conflicts, and anything it
contains that cannot work in this port. The same lines are in `mua3.log` (`[mods]`).

## Load order and turning mods off: `mods\order.txt`

```
# MUA3 mods, in load order: mods lower in the list win when two change the same file.
# A '-' in front of a name turns that mod off. New mod folders are added at the bottom.
Red Hulk
-Gamora Movie Recolor
Elektra 2005
```

- One mod per line, by folder name. When two mods change the same file, the one lower in the list wins (the log
  says `conflict: <file> from 'A' is replaced by 'B'`).
- `-` in front of a name turns that mod off; delete the `-` to turn it on again.
- New folders are added at the bottom, on. Lines for folders that are gone are removed.
- All mods off at once: `[mods] enabled=0` in `mua3.ini` (or the environment variable `MUA3_MODS=0`, for testing
  whether a problem comes from a mod).

## What mods can change

Any file of the game's RomFS. Files keep the exact path and name the game uses. Mods can also add files the game
does not have. The common ones:

| Files | What they hold |
| --- | --- |
| `ACTION\TEXTURE\*.g1t.ZL_` | character textures (retextures) |
| `ACTION\MODEL\*_mdl.bin.ZL_` | character models |
| `DATA\*.bin`, `ACTION\*.bin.ZL_` | data tables (characters, skills, items, ...) |
| `UI\screenlayout\...\*.kslt` | menu and HUD layouts and their textures |
| `UI\string\mes_*_<lang>.bin` | text |
| `SOUND\...` (`.ktsl2asbin`, ...) | sound and music |
| `MOVIE\*.mp4` | videos |

`.ZL_` files are zlib-compressed. The game reads the name it knows: a retexture saved as `X.g1t` where the game has
`X.g1t.ZL_` is never read. The log warns about such a pair (`the game reads X.g1t.ZL_: compressed`).

## What cannot work

- **exefs patches** (`exefs\` folders, `.ips`, `.pchtxt`): they patch the Switch program's ARM code. This port runs
  the game as native PC code built ahead of time, so there is no ARM code to patch. The log lists them as not
  supported; the mod's other files still load.
- **Cheats** (`cheats\<build id>.txt`, Atmosphere / emulator cheat codes): not supported.
- **DLC files**: mods for the base game's RomFS only (the DLC's own data is a separate package).
- Mods made for another game version than 4.0.1 (the one this port runs) may misbehave: use mods for the latest
  Switch update.

## Caches

- `romfs_index.bin` (the game's file list) holds the game's own files only. Turning mods on or off does not rebuild it.
- Shaders are cached by their content. A mod that changes shaders gets its shaders compiled the first time they are
  drawn (a short hitch once), then cached; the game's own cached shaders stay valid. Nothing needs clearing.
- Textures are cached in memory only, by what the game loads: a retexture of another size or format is a new texture.
  Textures in ASTC format are not decoded yet (shown grey); retextures in the formats the game uses (BC1 / BC3 /
  BC7, RGBA8) work.
- Saves: data-table mods (roster, items, unlocks) can write save data the unmodded game does not expect. Back up
  `save\` before trying such mods.

## For developers

- `src/core/hle/mods.{h,cpp}`: finding mods, `order.txt`, `mods.log`.
- `src/core/hle/services/fs.cpp` (`VirtualRomfs::ApplyMods`): reads the saved game index back into a tree, lays the
  enabled mods over it (same path, any case: replaced; new path: added, with its folders) and builds the RomFS image
  the game reads.
- `hle::ResolveRomfs(rel)`: the host file the game reads for a RomFS path. Code that reads game files itself (menu
  label textures in `menu_mods.cpp`) uses it, so it works on the same bytes as the game.
- For mod list UIs: `hle::mods::Scan(false)` (all mods in order, with `enabled`, `romfs` (empty: no files found),
  `layout`, `unsupported`), `hle::mods::SaveOrder(list)`, `hle::mods::Loaded()` (what this launch loaded, with
  replace / add counts) and `hle::mods::ChangedSinceLaunch()` ("restart to apply").
