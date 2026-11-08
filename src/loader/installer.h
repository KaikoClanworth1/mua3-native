#pragma once
#include <atomic>
#include <functional>
#include <mutex>
#include "common/common.h"

namespace loader {

struct InstallProgress {
    std::atomic<u64> bytes_done{0};
    std::atomic<u64> bytes_total{0};
    std::atomic<bool> cancel{false};
    std::mutex mutex;
    std::string stage;

    void SetStage(std::string s) {
        std::lock_guard lk(mutex);
        stage = std::move(s);
    }
    std::string Stage() {
        std::lock_guard lk(mutex);
        return stage;
    }
};

struct InstallOptions {
    fs::path isos_dir;   // folder with .nsp / .xci dumps (used when `files` is empty)
    fs::path keys_dir;   // folder with prod.keys (+ optional title.keys) (used when `keys_file` is empty)
    fs::path output_dir; // game data folder (exefs/, romfs/, control/, aoc/)
    bool extract_romfs = true;
    std::vector<fs::path> files; // the dumps to install, picked one by one (base game, update, DLC)
    fs::path keys_file;          // prod.keys picked directly (title.keys is looked for next to it)
};

// Scans the dumps, decrypts and extracts ExeFS, RomFS (with the update applied), control data and DLC.
bool InstallGame(const InstallOptions& opts, InstallProgress& progress, std::string& error);

// What one dump is (from its ticket / content meta names, without keys).
struct DumpInfo {
    fs::path path;
    std::string kind;     // "base game", "update", "DLC", "cartridge (base game)", "other game", "unknown"
    std::string title_id; // 16 hex digits when known
    u64 size = 0;
    bool base = false, update = false, dlc = false;
    bool ours = true;     // Marvel Ultimate Alliance 3 (title 010060700AC5xxxx), or unknown
};
DumpInfo ClassifyDump(const fs::path& path);

// Quick check of what's in the Isos folder, without needing keys.
struct DumpSummary {
    std::vector<std::string> lines;
    bool has_base = false, has_update = false;
    int dlc_count = 0;
};
DumpSummary ScanDumps(const fs::path& isos_dir);

bool IsInstalled(const fs::path& output_dir);

} // namespace loader
