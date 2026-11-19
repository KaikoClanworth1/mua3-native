// "Run diagnostics" (see diagnostics.h).
#include "setup/diagnostics.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <fstream>
#include <sstream>

#include <windows.h>

#include "launcher/report.h"

namespace diag {
namespace {

using Clock = std::chrono::steady_clock;

// The run: the title screen for a minute, then through the menus into a new game (the save folder is new and empty),
// then moving and attacking. `#` comments; see RunInputScript in app/main.cpp for the format.
std::string Script() {
    std::string s = "# MUA3 Native diagnostics run (written by the launcher)\n"
                    "# 0-60 s: the title screen as it is\n";
    // Each phase is a block that only moves forward from wherever the game is (a slow PC is still loading).
    // 60-170 s: the patch notes (Y: finish), the title (+), the first save slot (A), "Okay to save?" with CANCEL
    // first (LEFT: OKAY, A), the difficulty (A) and START GAME (A). (The title's UPDATE HISTORY is the L stick.)
    s += "# 60 s on: patch notes, title, new save, difficulty, start\n";
    const char* menu[] = {"Y", "PLUS", "A", "LEFT", "A"};
    int n = 0;
    for (double t = 60; t < kSkipFrom; t += 2.0)
        s += fmt::format("{:.1f} {}\n", t, menu[n++ % 5]);
    // 170-230 s: the opening movie: + shows the skip prompt, X skips; B closes the pause menu if + opened that
    // (as build\testinst\inst.ps1 togame does).
    s += "# skipping the opening movie\n";
    const char* skip[] = {"PLUS", "X", "B"};
    n = 0;
    for (double t = kSkipFrom; t < kPlayFrom; t += 2.5)
        s += fmt::format("{:.1f} {}\n", t, skip[n++ % 3]);
    // 230-326 s: in the stage: run in each direction, attack, jump, dodge
    s += "# moving and attacking\n";
    const int dirs[][2] = {{0, 32000}, {32000, 0}, {0, -32000}, {-32000, 0}};
    n = 0;
    for (double t = kPlayFrom; t < kScriptSeconds - 6; t += 6.0) {
        const auto& d = dirs[n++ % 4];
        s += fmt::format("{:.1f} STICK {} {} 2200\n", t, d[0], d[1]);
        s += fmt::format("{:.1f} Y 120\n", t + 2.5);
        s += fmt::format("{:.1f} Y 120\n", t + 3.0);
        s += fmt::format("{:.1f} X 120\n", t + 3.5);
        s += fmt::format("{:.1f} {} 120\n", t + 4.2, n % 2 ? "A" : "B");
        s += fmt::format("{:.1f} A 120\n", t + 5.2); // (also closes a dialog that opened)
    }
    s += fmt::format("{:.0f} QUIT\n", kScriptSeconds);
    return s;
}

std::string Stamp() {
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &now);
    char b[32];
    std::strftime(b, sizeof(b), "%Y-%m-%d_%H%M%S", &tm);
    return b;
}

// This process's environment for the game, MUA3_* switches of a developer's shell left out, plus the variant's.
std::wstring Environment(const Variant& v) {
    std::wstring out;
    if (wchar_t* env = GetEnvironmentStringsW()) {
        for (const wchar_t* p = env; *p; p += wcslen(p) + 1) {
            const std::wstring e = p;
            if (_wcsnicmp(e.c_str(), L"MUA3_", 5) == 0)
                continue;
            out += e;
            out.push_back(0);
        }
        FreeEnvironmentStringsW(env);
    }
    for (auto& [k, val] : v.env) {
        out += fs::u8path(k).wstring() + L"=" + fs::u8path(val).wstring();
        out.push_back(0);
    }
    out.push_back(0);
    return out;
}

u64 WriteTime(const fs::path& p) {
    WIN32_FILE_ATTRIBUTE_DATA a{};
    if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &a))
        return 0;
    return (u64(a.ftLastWriteTime.dwHighDateTime) << 32) | a.ftLastWriteTime.dwLowDateTime;
}

// mua3.ini without paths (folder names can hold the player's name): what the run played with.
std::string SettingsText(const fs::path& ini) {
    std::ifstream f(ini);
    std::string out, line;
    while (std::getline(f, line)) {
        std::string key = line.substr(0, line.find('='));
        std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        if (key.find("dir") != std::string::npos || key.find("path") != std::string::npos ||
            key.find("key") != std::string::npos || key.find("folder") != std::string::npos)
            continue;
        if (!line.empty())
            out += "  " + line + "\n";
    }
    return out;
}

double Field(const std::string& line, const char* before) {
    const size_t at = line.find(before);
    return at == std::string::npos ? NAN : std::atof(line.c_str() + at + strlen(before));
}

} // namespace

const std::vector<Variant>& Variants() {
    static const std::vector<Variant> v = {
        {"standard", "STANDARD", "Your settings as they are.", {}},
        {"tex_full", "TEXTURE UPLOAD: FULL", "Whole textures on every change (GitHub #8).", {{"MUA3_TEX_FULL", "1"}}},
        {"tex_staging", "TEXTURE UPLOAD: STAGING", "Textures through a staging copy (GitHub #8).", {{"MUA3_TEX_STAGING", "1"}}},
    };
    return v;
}

void Analyze(const fs::path& log, RunResult& r) {
    std::ifstream f(log);
    std::string line;
    double t_begin = -1, frame_ms_sum = 0;
    std::vector<std::pair<double, double>> mem; // (seconds, MB private)
    bool ended = false;
    while (std::getline(f, line)) {
        if (line.size() < 12 || line[0] != '[')
            continue;
        const double t = std::atof(line.c_str() + 1);
        if (line.find("] [ERROR] [") != std::string::npos || line.find("] [FATAL] [") != std::string::npos) {
            r.errors++;
            if (r.error_lines.size() < 6)
                r.error_lines.push_back(line.substr(0, 300));
        }
        // "[Texture:<thread>] upload path <path> (<source>)", once at startup (the texture variants)
        if (const size_t u = line.find("upload path "); u != std::string::npos && r.upload_path.empty() &&
                                                         line.find("[Texture:") != std::string::npos)
            r.upload_path = line.substr(u);
        if (line.find("diagnostics: begin") != std::string::npos)
            t_begin = t;
        else if (line.find("diagnostics: end") != std::string::npos)
            ended = true;
        else if (t_begin >= 0 && line.find("display: ") != std::string::npos) {
            const double n = Field(line, "display: "), avg = Field(line, "avg "), worst = Field(line, "worst ");
            const size_t w = line.find("worst ");
            const double late = w == std::string::npos ? NAN : Field(line.substr(w), " ms, ");
            if (!std::isnan(n) && !std::isnan(avg)) {
                r.frames += int(n);
                frame_ms_sum += n * avg;
                if (!std::isnan(worst))
                    r.worst_ms = std::max(r.worst_ms, worst);
                if (!std::isnan(late))
                    r.late += int(late);
            }
        } else if (t_begin >= 0 && line.find("memory: process ") != std::string::npos) {
            const double mb = Field(line, "memory: process ");
            if (!std::isnan(mb))
                mem.emplace_back(t - t_begin, mb);
            // "| textures: N re-uploads/s (X MB/s, ...": the render chat's texture rates (when the build logs them)
            if (const size_t tx = line.find("| textures: "); tx != std::string::npos) {
                const std::string rest = line.substr(tx);
                const double ups = Field(rest, "| textures: "), mbs = Field(rest, "re-uploads/s (");
                if (!std::isnan(ups) && !std::isnan(mbs))
                    r.tex_uploads += ups, r.tex_mb += mbs, r.tex_samples++;
            }
        }
    }
    if (r.frames)
        r.avg_ms = frame_ms_sum / r.frames;
    if (!mem.empty()) {
        r.start_mb = mem.front().second, r.end_mb = mem.back().second;
        for (auto& m : mem)
            r.peak_mb = std::max(r.peak_mb, m.second);
    }
    // The growth while playing (the stage part; loading the stage before it grows memory by design): least squares.
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    int n = 0;
    for (auto& [t, mb] : mem)
        if (t >= kPlayFrom + 10) {
            sx += t, sy += mb, sxx += t * t, sxy += t * mb;
            n++;
        }
    if (n >= 6 && n * sxx - sx * sx > 0) {
        r.slope = (n * sxy - sx * sy) / (n * sxx - sx * sx) * 60.0;
        r.slope_known = true;
    }
    r.finished = r.finished && ended;
    std::vector<std::string> why;
    if (r.crashed)
        why.push_back("crashed" + (r.crash.empty() ? std::string() : ": " + r.crash));
    else if (r.cancelled)
        why.push_back("cancelled");
    else if (r.stopped)
        why.push_back("did not finish in time (stopped)");
    else if (!r.finished)
        why.push_back(r.exit_code ? fmt::format("closed early with code {}", r.exit_code) : "closed before the script ended");
    if (r.slope_known && r.slope > kMaxSlopeMBPerMin)
        why.push_back(fmt::format("memory grows {:.0f} MB/min (limit {:.0f})", r.slope, kMaxSlopeMBPerMin));
    for (auto& e : r.error_lines)
        if (e.find("[FATAL]") != std::string::npos) {
            why.push_back("fatal stop");
            break;
        }
    r.pass = why.empty();
    r.why.clear();
    for (auto& w : why)
        r.why += (r.why.empty() ? "" : "; ") + w;
}

Runner::~Runner() { Stop(); }

void Runner::Stop() {
    cancel_ = true;
    if (thread_.joinable())
        thread_.join();
}

bool Runner::Start(const fs::path& install, const std::string& version, std::vector<int> variants) {
    if (running_ || variants.empty())
        return false;
    if (thread_.joinable())
        thread_.join();
    cancel_ = false;
    current = 0;
    total = int(variants.size());
    run_elapsed = 0;
    folder = install / "diag" / Stamp();
    report.clear();
    {
        std::lock_guard lk(mutex);
        results.clear();
        status = "Starting";
    }
    running_ = true;
    thread_ = std::thread([this, install, version, variants] { Thread(install, version, variants); });
    return true;
}

void Runner::Thread(fs::path install, std::string version, std::vector<int> variants) {
    std::error_code ec;
    fs::create_directories(folder, ec);
    LOG_INFO("Launcher", "diagnostics: {} runs into {}", variants.size(), folder.string());
    for (size_t i = 0; i < variants.size() && !cancel_; i++) {
        const Variant& v = Variants()[size_t(variants[i])];
        current = int(i) + 1;
        run_elapsed = 0;
        {
            std::lock_guard lk(mutex);
            status = "Starting the game";
        }
        RunResult r;
        r.id = v.id, r.label = v.label;
        const fs::path dir = folder / v.id;
        fs::create_directories(dir, ec);
        {
            std::ofstream(dir / "script.txt") << Script();
        }
        const fs::path exe = install / "mua3.exe";
        std::wstring cmd = L"\"" + exe.wstring() + L"\" --diagnostics \"" + dir.wstring() + L"\"";
        std::wstring env = Environment(v);
        STARTUPINFOW si{sizeof(si)};
        PROCESS_INFORMATION pi{};
        FILETIME launched{};
        GetSystemTimeAsFileTime(&launched);
        const u64 since = (u64(launched.dwHighDateTime) << 32) | launched.dwLowDateTime;
        const auto t0 = Clock::now();
        if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_UNICODE_ENVIRONMENT, env.data(),
                            install.c_str(), &si, &pi)) {
            r.why = "the game could not be started";
            std::lock_guard lk(mutex);
            results.push_back(r);
            continue;
        }
        CloseHandle(pi.hThread);
        LOG_INFO("Launcher", "diagnostics run {} ({}): pid {}", i + 1, v.id, pi.dwProcessId);
        {
            std::lock_guard lk(mutex);
            status = "Playing";
        }
        while (WaitForSingleObject(pi.hProcess, 250) == WAIT_TIMEOUT) {
            run_elapsed = std::chrono::duration<double>(Clock::now() - t0).count();
            if (cancel_ || run_elapsed > kScriptSeconds + kOvertimeSeconds) { // (only the game this run started)
                r.stopped = true;
                r.cancelled = cancel_;
                TerminateProcess(pi.hProcess, 1);
                WaitForSingleObject(pi.hProcess, 10000);
                break;
            }
        }
        GetExitCodeProcess(pi.hProcess, &r.exit_code);
        CloseHandle(pi.hProcess);
        r.seconds = std::chrono::duration<double>(Clock::now() - t0).count();
        r.finished = !r.stopped && r.exit_code == 0;
        {
            std::lock_guard lk(mutex);
            status = "Reading the log";
        }
        // this run's files: the log, crash files written since it started
        fs::copy_file(install / "mua3.log", dir / "mua3.log", fs::copy_options::overwrite_existing, ec);
        for (auto& e : fs::directory_iterator(install / "crashes", ec))
            if (e.is_regular_file(ec) && WriteTime(e.path()) >= since) {
                fs::copy_file(e.path(), dir / e.path().filename(), fs::copy_options::overwrite_existing, ec);
                if (e.path().extension() == ".txt" && r.crash.empty()) {
                    std::ifstream cf(e.path());
                    std::getline(cf, r.crash);
                    r.crashed = true;
                }
            }
        if (r.exit_code == 3221225477u) // (0xC0000005 with no crash file)
            r.crashed = true;
        Analyze(dir / "mua3.log", r);
        // the run's own save folder goes (the player's save was never used)
        fs::remove_all(dir / "save", ec);
        LOG_INFO("Launcher", "diagnostics run {} ({}): {}{}", i + 1, v.id, r.pass ? "PASS" : "FAIL", r.why.empty() ? "" : " - " + r.why);
        std::lock_guard lk(mutex);
        results.push_back(r);
    }

    // report.txt
    std::string t = "MUA3 Native diagnostics report\n"
                    "==============================\n";
    t += "Version: " + version + "\nFolder: " + folder.filename().string() + "\n\n";
    t += "System:\n" + ::report::SystemInfo() + "\n";
    {
        std::lock_guard lk(mutex);
        int passed = 0;
        for (auto& r : results)
            passed += r.pass;
        t += fmt::format("Result: {} of {} runs passed{}\n\n", passed, results.size(), cancel_ ? " (cancelled)" : "");
        for (auto& r : results) {
            t += fmt::format("[{}] {} ({})\n", r.pass ? "PASS" : "FAIL", r.label, r.id);
            if (!r.why.empty())
                t += "  why: " + r.why + "\n";
            t += fmt::format("  run time: {:.0f} s, exit code {}{}\n", r.seconds, r.exit_code, r.finished ? ", script finished" : "");
            if (r.slope_known)
                t += fmt::format("  memory: {:.0f} MB/min while playing (limit {:.0f}); {:.0f} -> {:.0f} MB, peak {:.0f} MB\n",
                                 r.slope, kMaxSlopeMBPerMin, r.start_mb, r.end_mb, r.peak_mb);
            else
                t += fmt::format("  memory: not enough samples while playing; {:.0f} -> {:.0f} MB, peak {:.0f} MB\n", r.start_mb,
                                 r.end_mb, r.peak_mb);
            if (r.frames)
                t += fmt::format("  frames: {}, average {:.2f} ms ({:.0f} fps), worst {:.1f} ms, {} over 20 ms\n", r.frames,
                                 r.avg_ms, r.avg_ms > 0 ? 1000.0 / r.avg_ms : 0.0, r.worst_ms, r.late);
            if (!r.upload_path.empty())
                t += "  texture " + r.upload_path + "\n";
            if (r.tex_samples)
                t += fmt::format("  textures: {:.1f} re-uploads/s, {:.1f} MB/s on average (from the 5 s memory lines)\n",
                                 r.tex_uploads / r.tex_samples, r.tex_mb / r.tex_samples);
            if (!r.crash.empty())
                t += "  crash: " + r.crash + "\n";
            if (r.errors) {
                t += fmt::format("  errors in the log: {}\n", r.errors);
                for (auto& e : r.error_lines)
                    t += "    " + e + "\n";
            }
            t += "  log: " + r.id + "\\mua3.log\n\n";
        }
    }
    t += "Settings (mua3.ini, folders left out):\n" + SettingsText(install / "mua3.ini");
    const fs::path rp = folder / "report.txt";
    {
        std::ofstream(rp, std::ios::binary) << t;
    }
    report = rp;
    {
        std::lock_guard lk(mutex);
        status = "Done";
    }
    LOG_INFO("Launcher", "diagnostics report: {}", rp.string());
    running_ = false;
}

} // namespace diag
