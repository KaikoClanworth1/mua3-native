// "Run diagnostics" (launcher SETTINGS): the game plays a fixed script of about 5 minutes once per chosen variant,
// with the player's own settings, from its own empty save folder (mua3.exe --diagnostics refuses the real one).
// Afterwards a short report (pass/fail per run, memory growth, frame times, crashes, GPU and driver) and each run's
// log are in <install>\diag\<time>\ for the player to attach to an issue. Nothing is sent anywhere.
#pragma once
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "common/common.h"

namespace diag {

// The pass / fail limits (the same as the idle test runner's).
constexpr double kMaxSlopeMBPerMin = 50.0;
// The script's phases (seconds from the start): menus into a new game, skipping the opening movie, playing, QUIT.
constexpr double kSkipFrom = 170.0, kPlayFrom = 230.0;
constexpr double kScriptSeconds = 330.0;   // the script's QUIT
constexpr double kOvertimeSeconds = 150.0; // loading on a slow PC; after this the run is stopped and fails

struct Variant {
    const char* id;    // folder name
    const char* label; // shown
    const char* sub;
    std::vector<std::pair<const char*, const char*>> env; // set for this run only (the game's own process)
};
const std::vector<Variant>& Variants();

struct RunResult {
    std::string id, label;
    bool finished = false;  // the script reached its end and the game closed by itself
    bool crashed = false;
    bool stopped = false;   // overtime or cancelled: the launcher ended it
    bool cancelled = false;
    unsigned long exit_code = 0;
    std::string crash;      // the crash headline
    double seconds = 0;     // run time
    bool slope_known = false;
    double slope = 0;       // MB/min of process private memory, in the stage part of the run
    double start_mb = 0, end_mb = 0, peak_mb = 0;
    double avg_ms = 0, worst_ms = 0;
    int frames = 0, late = 0; // late: over 20 ms
    std::string upload_path; // the game's "upload path ..." line (texture cache)
    double tex_uploads = 0, tex_mb = 0; // sums of the 5 s texture rates
    int tex_samples = 0;
    int errors = 0;
    std::vector<std::string> error_lines; // the first few
    bool pass = false;
    std::string why; // why it failed
};

class Runner {
public:
    ~Runner();
    // `variants`: indexes into Variants(). False if a run is already going.
    bool Start(const fs::path& install, const std::string& version, std::vector<int> variants);
    void Cancel() { cancel_ = true; }
    void Stop(); // cancel and wait (the report of what ran is still written)
    bool Running() const { return running_; }

    // progress (any thread)
    std::atomic<int> current{0}, total{0};
    std::atomic<double> run_elapsed{0}; // seconds into the current run
    fs::path folder;                    // <install>\diag\<time> (set by Start)
    fs::path report;                    // report.txt, once written
    std::mutex mutex;
    std::vector<RunResult> results;     // (mutex)
    std::string status;                 // (mutex) e.g. "Starting the game"

private:
    void Thread(fs::path install, std::string version, std::vector<int> variants);
    std::thread thread_;
    std::atomic<bool> running_{false}, cancel_{false};
};

// Reads one run's mua3.log into `r` (timing, memory, errors) and sets pass / why. Public for testing.
void Analyze(const fs::path& log, RunResult& r);

} // namespace diag
