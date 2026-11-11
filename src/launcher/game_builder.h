// Builds mua3.exe on the player's PC from their own installed game: the game's code is unpacked from the install,
// translated to C++ by recomp.exe, compiled with the player's MSVC, and linked with the port's prebuilt libraries
// (the release's sdk/ folder). The release itself contains none of the game.
#pragma once
#include <atomic>
#include <mutex>
#include <string>
#include <vector>

#include "common/common.h"

namespace builder {

// The installed Microsoft C++ tools (Visual Studio or Build Tools), as vswhere reports them.
struct Toolchain {
    bool ok = false;
    fs::path vcvars;        // ...\VC\Auxiliary\Build\vcvars64.bat
    std::string version;    // cl.exe version, e.g. "19.51.36248"
    std::string problem;    // why it cannot be used (missing, too old)
};
// Minimum compiler: the one the prebuilt libraries were made with (an older linker cannot read them).
constexpr int kMinCompiler = _MSC_VER; // e.g. 1951 = MSVC 19.51 (Visual Studio 2026)
Toolchain FindToolchain();

// Where the build reads and writes.
struct Paths {
    fs::path game_dir; // installed game data (exefs/ is read)
    fs::path sdk_dir;  // the release's sdk/: include/, lib/, obj/, recomp/ (lists), tools/recomp.exe
    fs::path work_dir; // nso/, gen/ (generated C++), obj/ (compiled), logs
    fs::path out_exe;  // mua3.exe to write
    fs::path icon;     // optional .ico (made from the player's own game) embedded in mua3.exe
    bool windowed = false; // link as a Windows app (no console window: started from a shortcut like any PC game)
    fs::path prebuilt_code; // developers only: link this library of already compiled game code (the dev tree's
                            // nxrecomp_full.lib) instead of translating and compiling the game (tests the rest quickly)
};
// The release layout next to the launcher, or the source tree when run from build/bin (developers).
Paths DefaultPaths(const fs::path& game_dir);

struct Progress {
    std::atomic<bool> running{false}, finished{false}, ok{false}, cancel{false};
    std::atomic<int> done{0}, total{0}; // compiled shards
    std::mutex mutex;
    std::string stage, error;
    std::vector<std::string> log; // last lines, for the launcher
    void Stage(std::string s);
    void Log(std::string s);
    std::string GetStage();
    std::string GetError();
    std::vector<std::string> Tail(size_t n);
};

// Runs every step (blocking; the launcher calls it on a worker thread). `jobs` compilers run at once.
bool BuildGame(const Paths& paths, int jobs, Progress& progress);
// Compiler jobs that fit this PC: cores - 1, at most one per 2.5 GB of RAM.
int DefaultJobs();
// True when a built mua3.exe is newer than everything it was built from.
bool GameBuilt(const Paths& paths);

} // namespace builder
