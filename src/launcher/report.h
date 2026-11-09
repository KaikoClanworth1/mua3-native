// "Report a problem": a zip of what is needed to look into a crash or a bug, made on the player's PC for them to
// attach to an issue: their description, system and version information, settings, install info, logs, the newest
// crash files and the game build log. Never keys, saves or game files.
#pragma once
#include <optional>
#include <string>
#include <vector>

#include "common/common.h"

namespace report {

// The newest crash summary (crashes/*.txt) of the game or the launcher, and its first lines.
struct Crash {
    fs::path summary;
    std::string headline; // e.g. "mua3 crashed: unhandled exception / access violation ... at mua3.exe+0x1234"
    u64 time = 0;         // file time (for "already reported")
};
std::optional<Crash> NewestCrash();

// What goes in the zip (shown to the player before it is made).
struct Item {
    fs::path path;   // on disk
    std::string name; // in the zip
    u64 size = 0;
};
std::vector<Item> Collect(const fs::path& game_dir, const fs::path& build_dir);

struct Result {
    bool ok = false;
    fs::path zip;
    std::string error;
};
// `about` = the description typed by the player; `extra` = lines the launcher adds (displays, C++ tools, settings).
Result Create(const std::string& about, const std::vector<std::string>& extra, const fs::path& game_dir,
              const fs::path& build_dir);

// The system summary that starts report.txt (Windows, CPU, memory, GPU and driver).
std::string SystemInfo();

} // namespace report
