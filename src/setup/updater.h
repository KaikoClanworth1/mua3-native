// The launcher's updater: asks GitHub for the newest release of the port, downloads its package (the same zip players
// download by hand), unpacks it under %LOCALAPPDATA%\MUA3Native\packages\<version> and starts its MUA3Setup.exe with
// --update, which rebuilds the installed game with the new version (the player's own game files and keys, as
// UPDATE OR REPAIR does). The package holds no part of the game.
//
// Developer tests: MUA3_UPDATE_API=<url or local .json file> replaces GitHub's answer, MUA3_UPDATE_CURRENT=<version>
// replaces this build's version (a "dev" build checks nothing without it), MUA3_UPDATE_START=1 starts the update
// without a click.
#pragma once
#include <atomic>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "common/common.h"

namespace updater {

struct Release {
    std::string version; // "v0.3.5"
    std::string url;     // the Windows package (.zip)
    u64 size = 0;
};

// Numbers of a version ("v0.3.4", "v0.3.4-12-gabc", "0.3"): true when `a` is newer than `b`. False when either has none.
bool Newer(const std::string& a, const std::string& b);

// GitHub's latest release (blocking, about a second; a 20 s limit). Empty with `error` set when it can't tell.
std::optional<Release> Latest(std::string& error);

// The whole update, on its own thread: check, then (Start) download, unpack, run the new setup.
struct Updater {
    enum class State { Idle, Checking, UpToDate, Available, Downloading, Unpacking, Ready, Failed };
    std::atomic<State> state{State::Idle};
    std::atomic<float> progress{0};
    Release release;     // (once Available)
    std::string error;   // (mutex) why it failed
    fs::path setup_exe;  // (once Ready) the new package's MUA3Setup.exe
    std::mutex mutex;

    // Looks for a newer release than `current` (this build's version). Nothing happens for a "dev" build.
    void Check(const std::string& current);
    // Downloads and unpacks the release found (State::Available). `keep`: a package folder still in use (the
    // install's source_dir), never removed; older packages are.
    void Start(const fs::path& keep);
    // Runs the new setup (State::Ready) to update `install_dir`; true when it started.
    bool Run();
    ~Updater();

private:
    std::thread thread;
    void Join();
};

} // namespace updater
