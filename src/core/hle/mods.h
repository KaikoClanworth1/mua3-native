// Mods: loose game files that replace or add to the game's RomFS (docs/mods.md).
//
// Every folder in mods\ (next to mua3.exe) is one mod. Its RomFS files are found in any of the layouts players get
// mods in: <mod>\romfs\..., <mod>\010060700AC50000\romfs\..., <mod>\atmosphere\contents\010060700AC50000\romfs\...,
// <mod>\contents\010060700AC50000\romfs\..., or the RomFS folders (DATA, UI, ...) straight in <mod>. A whole
// emulator mod folder (mods\010060700AC50000\<mod>\romfs) counts as one mod per subfolder.
// mods\order.txt holds the load order (top to bottom, later mods win) and which mods are off ('-' in front).
// The RomFS layer (services/fs.cpp) merges the enabled mods once, when the game first opens its RomFS: changes
// apply at the next launch.
#pragma once
#include <string>
#include <string_view>
#include <vector>

#include "common/common.h"

namespace hle::mods {

constexpr const char* kTitleId = "010060700AC50000";

struct Mod {
    std::string name;    // folder name as listed in order.txt (for the emulator layout: the subfolder's name)
    fs::path folder;     // mods\<name> (or mods\010060700AC50000\<name>)
    fs::path romfs;      // the folder holding its RomFS files; empty: none found
    std::string layout;  // which layout it was found in, for the log
    bool enabled = true;
    std::vector<std::string> unsupported; // parts that cannot apply natively (exefs patches, cheats)
    // filled in by the RomFS layer at launch
    bool loaded = false;
    int overrides = 0, addons = 0;
};

// The mods folder (mods\ in the working directory, next to mua3.exe and mua3.ini).
fs::path ModsDir();

// Off with [mods] enabled=0 in mua3.ini, or MUA3_MODS=0.
bool Enabled();

// Scans mods\ and reads mods\order.txt: every mod in load order, enabled or not. With `update_order`, mods not yet
// listed are appended (on, by name), listed folders that are gone are dropped, and order.txt is rewritten when that
// changed anything (it is created on first use).
std::vector<Mod> Scan(bool update_order);

// Writes mods\order.txt from `mods` (their order and enabled flags). For the mod list UIs.
bool SaveOrder(const std::vector<Mod>& mods);

// ---- for menus: what this launch loaded (empty before the game opened its RomFS, or with mods off)
std::vector<Mod> Loaded();
// order.txt or the mods folder differ from what this launch loaded: the menu says "restart to apply".
bool ChangedSinceLaunch();

// The host file for RomFS path `rel` without the RomFS layer (for code that runs before the game starts): the last
// enabled mod's copy, else game_dir\romfs\rel.
fs::path ResolveLoose(const fs::path& game_dir, std::string_view rel, bool mods_enabled);

// ---- for the RomFS layer
void SetLoaded(std::vector<Mod> mods);
void WriteLog(const std::vector<Mod>& mods, const std::vector<std::string>& lines);

} // namespace hle::mods

namespace hle {
// The host file the game reads for RomFS path `rel` ("UI/screenlayout/x.kslt"): a mod's file when an enabled mod
// replaces it, else the extracted game file. Code that reads game files directly (not through fsp-srv) uses this,
// so it sees the same bytes as the game.
fs::path ResolveRomfs(std::string_view rel);
} // namespace hle
