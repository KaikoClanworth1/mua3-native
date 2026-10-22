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
    fs::path isos_dir;   // folder with .nsp / .xci dumps
    fs::path keys_dir;   // folder with prod.keys (+ optional title.keys)
    fs::path output_dir; // game data folder (exefs/, romfs/, control/, aoc/)
    bool extract_romfs = true;
};

// Scans the dumps, decrypts and extracts ExeFS, RomFS (with the update applied), control data and DLC.
bool InstallGame(const InstallOptions& opts, InstallProgress& progress, std::string& error);

// Quick check of what's in the Isos folder, without needing keys.
struct DumpSummary {
    std::vector<std::string> lines;
    bool has_base = false, has_update = false;
    int dlc_count = 0;
};
DumpSummary ScanDumps(const fs::path& isos_dir);

bool IsInstalled(const fs::path& output_dir);

} // namespace loader
