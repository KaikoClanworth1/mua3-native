#include "launcher/game_builder.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <fstream>
#include <optional>
#include <thread>

#include <windows.h>

#include "loader/formats.h"

namespace builder {

namespace {

// Where each module of the game sits and how much of it is code (the layout the port loads it at), and main's
// build id: the port supports the last update (4.0.1).
struct Module {
    const char* name;
    const char* base;
    const char* size;
};
constexpr Module kModules[] = {
    {"rtld", "8000000", "19f0"},       {"main", "8004000", "159d340"},  {"sdk", "df54000", "51c600"},
    {"subsdk0", "cfc5000", "377110"},  {"subsdk1", "d6e5000", "4ce300"},
};
constexpr u8 kMainBuildId[8] = {0xe8, 0x53, 0xc4, 0x4f, 0xdf, 0x18, 0xb8, 0x8f};

const char* const kCompileFlags = "/c /nologo /O2 /Ob3 /GS- /EHsc /MT /std:c++20 /permissive- /bigobj /utf-8 /DNDEBUG";
const char* const kLibs[] = {
    // the port (sdk/lib)
    "nxcore.lib", "platform.lib", "nxloader.lib", "fmt.lib",
    "imgui.lib", "SDL3-static.lib",
    // Windows
    "dbghelp.lib", "bcrypt.lib", "mfplat.lib", "mfreadwrite.lib", "mfuuid.lib", "kernel32.lib", "user32.lib",
    "gdi32.lib", "imm32.lib", "ole32.lib", "oleaut32.lib", "version.lib", "uuid.lib", "advapi32.lib", "setupapi.lib",
    "shell32.lib", "dinput8.lib", "d3d11.lib", "dxgi.lib", "d3dcompiler.lib", "xinput.lib", "xaudio2.lib", "winmm.lib",
    "winspool.lib", "comdlg32.lib", "windowscodecs.lib",
};

std::wstring W(const std::string& s) { return fs::u8path(s).wstring(); }
std::string Q(const fs::path& p) { return "\"" + p.string() + "\""; }

// Runs a program hidden, its output into `log` (appended), and returns its exit code (-1: could not start).
int Run(const std::wstring& cmdline, const std::vector<wchar_t>* env, const fs::path& log, DWORD priority,
        std::atomic<bool>* cancel = nullptr, std::string* captured = nullptr) {
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE out = INVALID_HANDLE_VALUE, rd = nullptr, wr = nullptr;
    if (captured) {
        if (!CreatePipe(&rd, &wr, &sa, 0))
            return -1;
        SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
        out = wr;
    } else {
        out = CreateFileW(log.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_ALWAYS,
                          FILE_ATTRIBUTE_NORMAL, nullptr);
    }
    STARTUPINFOW si{sizeof(si)};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = out;
    si.hStdError = out;
    si.hStdInput = nullptr;
    PROCESS_INFORMATION pi{};
    std::wstring cmd = cmdline;
    const BOOL started = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE,
                                        CREATE_NO_WINDOW | priority | (env ? CREATE_UNICODE_ENVIRONMENT : 0),
                                        env ? (void*)env->data() : nullptr, nullptr, &si, &pi);
    if (captured) {
        CloseHandle(wr);
        if (started) {
            char buf[4096];
            DWORD n = 0;
            while (ReadFile(rd, buf, sizeof(buf), &n, nullptr) && n)
                captured->append(buf, n);
        }
        CloseHandle(rd);
    } else if (out != INVALID_HANDLE_VALUE) {
        CloseHandle(out);
    }
    if (!started)
        return -1;
    while (WaitForSingleObject(pi.hProcess, 200) == WAIT_TIMEOUT)
        if (cancel && *cancel) {
            TerminateProcess(pi.hProcess, 1);
            WaitForSingleObject(pi.hProcess, 5000);
            break;
        }
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return int(code);
}

// The environment vcvars64.bat sets up (PATH, INCLUDE, LIB, ...), as a CreateProcess block.
bool CaptureEnvironment(const fs::path& vcvars, std::vector<wchar_t>& block, std::wstring& tools_dir) {
    std::string out;
    const std::wstring cmd = L"cmd.exe /u /c \"\"" + vcvars.wstring() + L"\" >nul 2>&1 && set\"";
    if (Run(cmd, nullptr, {}, NORMAL_PRIORITY_CLASS, nullptr, &out) != 0 || out.size() < 2)
        return false;
    const std::wstring text(reinterpret_cast<const wchar_t*>(out.data()), out.size() / 2);
    block.clear();
    size_t at = 0;
    while (at < text.size()) {
        size_t end = text.find(L'\n', at);
        if (end == std::wstring::npos)
            end = text.size();
        std::wstring line = text.substr(at, end - at);
        if (!line.empty() && line.back() == L'\r')
            line.pop_back();
        if (line.find(L'=') != std::wstring::npos && line[0] != L'=') {
            if (_wcsnicmp(line.c_str(), L"VCToolsInstallDir=", 18) == 0)
                tools_dir = line.substr(18);
            block.insert(block.end(), line.begin(), line.end());
            block.push_back(0);
        }
        at = end + 1;
    }
    block.push_back(0);
    return !tools_dir.empty();
}

u64 MTime(const fs::path& p) {
    std::error_code ec;
    const auto t = fs::last_write_time(p, ec);
    return ec ? 0 : u64(t.time_since_epoch().count());
}

} // namespace

void Progress::Stage(std::string s) {
    std::lock_guard lk(mutex);
    stage = s;
    log.push_back("== " + s);
}
void Progress::Log(std::string s) {
    std::lock_guard lk(mutex);
    log.push_back(std::move(s));
    if (log.size() > 400)
        log.erase(log.begin(), log.begin() + 100);
}
std::string Progress::GetStage() {
    std::lock_guard lk(mutex);
    return stage;
}
std::string Progress::GetError() {
    std::lock_guard lk(mutex);
    return error;
}
std::vector<std::string> Progress::Tail(size_t n) {
    std::lock_guard lk(mutex);
    return {log.end() - std::min(n, log.size()), log.end()};
}

Toolchain FindToolchain() {
    Toolchain t;
    const fs::path vswhere = fs::path(_wgetenv(L"ProgramFiles(x86)") ? _wgetenv(L"ProgramFiles(x86)") : L"C:\\Program Files (x86)") /
                             "Microsoft Visual Studio" / "Installer" / "vswhere.exe";
    if (!fs::exists(vswhere)) {
        t.problem = "Microsoft C++ Build Tools are not installed.";
        return t;
    }
    std::string out;
    Run(L"\"" + vswhere.wstring() + L"\" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 "
        L"-property installationPath -utf8", nullptr, {}, NORMAL_PRIORITY_CLASS, nullptr, &out);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r'))
        out.pop_back();
    if (out.empty()) {
        t.problem = "Microsoft C++ Build Tools are not installed (no Visual Studio with C++ found).";
        return t;
    }
    const fs::path vs = fs::u8path(out);
    t.vcvars = vs / "VC" / "Auxiliary" / "Build" / "vcvars64.bat";
    std::ifstream ver(vs / "VC" / "Auxiliary" / "Build" / "Microsoft.VCToolsVersion.default.txt");
    std::string tools; // 14.51.36248: compiler 19.51
    std::getline(ver, tools);
    int major = 0, minor = 0;
    std::sscanf(tools.c_str(), "%d.%d", &major, &minor);
    t.version = tools.size() > 2 ? "19" + tools.substr(2) : tools; // tools 14.51.x = compiler 19.51.x
    if (!fs::exists(t.vcvars) || major != 14) {
        t.problem = "The Visual Studio installation has no usable C++ tools.";
        return t;
    }
    if (1900 + minor < kMinCompiler) {
        t.problem = fmt::format("The C++ tools are too old (MSVC {}): Build Tools for Visual Studio 2026 or newer are needed.",
                                t.version);
        return t;
    }
    t.ok = true;
    return t;
}

Paths DefaultPaths(const fs::path& game_dir) {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    const fs::path dir = fs::path(exe).parent_path();
    Paths p;
    p.game_dir = game_dir;
    p.sdk_dir = dir / "sdk";
    p.work_dir = dir / "build_game";
    p.out_exe = dir / "mua3.exe";
    return p;
}

int DefaultJobs() {
    MEMORYSTATUSEX ms{sizeof(ms)};
    GlobalMemoryStatusEx(&ms);
    const int by_ram = std::max(1, int(ms.ullTotalPhys / (2560ull << 20)));
    const int cores = std::max(1, int(std::thread::hardware_concurrency()) - 1);
    return std::clamp(std::min(cores, by_ram), 1, 32);
}

bool GameBuilt(const Paths& p) {
    return fs::exists(p.out_exe) && MTime(p.out_exe) >= MTime(p.sdk_dir / "lib" / "nxcore.lib");
}

bool BuildGame(const Paths& p, int jobs, Progress& prog) {
    prog.running = true;
    prog.finished = false;
    prog.ok = false;
    prog.done = 0;
    prog.total = 0;
    std::error_code ec;
    fs::create_directories(p.work_dir, ec);
    const fs::path log = p.work_dir / "build.log";
    fs::remove(log, ec);
    auto fail = [&](std::string why) {
        {
            std::lock_guard lk(prog.mutex);
            prog.error = why;
        }
        prog.Log("FAILED: " + why);
        std::ofstream(log, std::ios::app) << "FAILED: " << why << "\n";
        prog.running = false;
        prog.finished = true;
        return false;
    };
    auto note = [&](const std::string& s) {
        prog.Log(s);
        std::ofstream(log, std::ios::app) << s << "\n";
    };
    auto stage = [&](const std::string& s) {
        prog.Stage(s);
        std::ofstream(log, std::ios::app) << "== " << s << "\n";
    };
    const auto t0 = std::chrono::steady_clock::now();

    // 1. the C++ tools
    stage("Checking the C++ tools");
    const Toolchain tc = FindToolchain();
    if (!tc.ok)
        return fail(tc.problem);
    std::vector<wchar_t> env;
    std::wstring tools_dir;
    if (!CaptureEnvironment(tc.vcvars, env, tools_dir))
        return fail("Could not set up the C++ tools (vcvars64.bat).");
    const fs::path bin = fs::path(tools_dir) / "bin" / "Hostx64" / "x64";
    const fs::path cl = bin / "cl.exe", link = bin / "link.exe";
    if (!fs::exists(cl) || !fs::exists(link))
        return fail("cl.exe / link.exe not found in " + bin.string());
    note(fmt::format("MSVC {} at {}", tc.version, bin.string()));
    for (const char* need : {"include/core/cpu/recomp/recomp_rt.h", "lib/nxcore.lib", "obj/main.obj", "tools/recomp.exe"})
        if (!fs::exists(p.sdk_dir / need))
            return fail("The release is incomplete: " + (p.sdk_dir / need).string() + " is missing.");

    std::vector<fs::path> shards;
    const fs::path obj = p.work_dir / "obj";
    if (!p.prebuilt_code.empty()) { // developers: the game's code compiled already (steps 2-4 skipped)
        if (!fs::exists(p.prebuilt_code))
            return fail("Prebuilt code not found: " + p.prebuilt_code.string());
        stage("Compiling the game (developer: prebuilt " + p.prebuilt_code.filename().string() + ")");
        note("Translating and compiling skipped: linking " + p.prebuilt_code.string());
    } else {
    // 2. the game's code, from the install
    stage("Unpacking the game's code from your install");
    const fs::path nso = p.work_dir / "nso";
    fs::create_directories(nso, ec);
    for (const Module& m : kModules) {
        const auto data = common::ReadFile(p.game_dir / "exefs" / m.name);
        if (data.empty())
            return fail(fmt::format("{} is missing: install the game first.", (p.game_dir / "exefs" / m.name).string()));
        auto img = loader::LoadNso(data);
        if (!img)
            return fail(fmt::format("Could not read {} from the install.", m.name));
        if (std::string(m.name) == "main" && std::memcmp(img->module_id.data(), kMainBuildId, 8) != 0)
            return fail("The installed game is not update 4.0.1, the version this port supports. Install the 4.0.1 "
                        "update with the game.");
        common::WriteFile(nso / (std::string(m.name) + ".bin"), img->image);
    }
    if (prog.cancel)
        return fail("Cancelled.");

    // 3. translate it to C++
    stage("Translating the game's code to C++ (several minutes)");
    const fs::path gen = p.work_dir / "gen";
    fs::create_directories(gen, ec);
    std::string args = Q(p.sdk_dir / "tools" / "recomp.exe") + " " + Q(gen);
    for (const Module& m : kModules)
        args += " " + Q(nso / (std::string(m.name) + ".bin")) + " " + m.base + " " + m.size;
    std::vector<fs::path> lists;
    for (auto& e : fs::directory_iterator(p.sdk_dir / "recomp", ec))
        lists.push_back(e.path());
    std::sort(lists.begin(), lists.end());
    for (auto& l : lists) {
        const std::string n = l.filename().string();
        if (n.rfind("prof", 0) == 0)
            args += " --profile " + Q(l);
        else if (n.rfind("missing", 0) == 0)
            args += " --missing " + Q(l);
    }
    args += " --all 1 --sweep 1 --noopt " + Q(p.sdk_dir / "recomp" / "noopt.txt") + " --skip " +
            Q(p.sdk_dir / "recomp" / "skip.txt") + " --nogm " + Q(p.sdk_dir / "recomp" / "nogm.txt");
    if (fs::exists(p.sdk_dir / "recomp" / "hugeopt.txt"))
        args += " --hugeopt " + Q(p.sdk_dir / "recomp" / "hugeopt.txt");
    if (const int rc = Run(W(args), nullptr, log, BELOW_NORMAL_PRIORITY_CLASS, &prog.cancel); rc != 0)
        return fail(prog.cancel ? "Cancelled." : fmt::format("recomp.exe failed (code {}); see {}", rc, log.string()));

    // 4. compile it
    for (auto& e : fs::directory_iterator(gen, ec)) {
        const std::string n = e.path().filename().string();
        if (n.rfind("recomp_", 0) == 0 && e.path().extension() == ".cpp")
            shards.push_back(e.path());
    }
    std::sort(shards.begin(), shards.end());
    if (shards.empty())
        return fail("recomp.exe produced no code.");
    fs::create_directories(obj, ec);
    prog.total = int(shards.size());
    stage(fmt::format("Compiling the game ({} parts, {} at a time)", shards.size(), jobs));
    const u64 header_time = MTime(p.sdk_dir / "include" / "core" / "cpu" / "recomp" / "recomp_rt.h");
    // Big functions (300 KB .. 1 MB of C++) come in files of their own, recomp_<shard>_<entry>.cpp: MSVC compiles them
    // with no optimisation at all unless told to (/d2OptimizeHugeFunctions). The flag costs minutes and about 5 GB per
    // MB of C++, so only the hot ones get it (hugeopt.txt; recomp marks their files' first line). Those start first,
    // together within half the RAM free now; everything else follows largest first, so no long part is left to the end.
    auto is_huge = [](const fs::path& f) {
        if (f.stem().string().find('_', 7) == std::string::npos)
            return false;
        std::ifstream in(f);
        std::string first;
        std::getline(in, first);
        return first.rfind("// mua3: optimise huge", 0) == 0;
    };
    std::vector<size_t> order(shards.size());
    std::vector<u64> need(shards.size(), 0), size(shards.size(), 0); // need: estimated bytes, marked files only
    for (size_t i = 0; i < shards.size(); i++) {
        order[i] = i;
        size[i] = fs::file_size(shards[i], ec);
        if (is_huge(shards[i]))
            need[i] = std::max<u64>(1ull << 30, u64(double(size[i]) * 5.0 * 1024.0));
    }
    std::stable_sort(order.begin(), order.end(),
                     [&](size_t a, size_t b) { return need[a] != need[b] ? need[a] > need[b] : size[a] > size[b]; });
    MEMORYSTATUSEX ms{sizeof(ms)};
    GlobalMemoryStatusEx(&ms);
    const u64 budget = ms.ullAvailPhys / 2;
    const int big_total = int(std::count_if(need.begin(), need.end(), [](u64 n) { return n != 0; }));
    std::atomic<int> big_done{0};
    if (big_total)
        note(fmt::format("{} large functions to optimise, within {:.1f} GB of memory", big_total, budget / double(1ull << 30)));
    std::mutex sched_mutex;
    std::condition_variable sched_cv;
    std::vector<bool> taken(shards.size(), false);
    u64 reserved = 0; // memory of the big files compiling now
    std::atomic<bool> broken{false};
    std::string broken_why;
    std::mutex broken_mutex;
    // the next part to compile: the first one not taken whose memory fits (one big file always may start alone)
    auto take = [&]() -> std::optional<size_t> {
        std::unique_lock lk(sched_mutex);
        for (;;) {
            if (broken || prog.cancel)
                return std::nullopt;
            bool left = false;
            for (size_t i : order) {
                if (taken[i])
                    continue;
                left = true;
                if (need[i] && reserved && reserved + need[i] > budget)
                    continue;
                taken[i] = true;
                reserved += need[i];
                return i;
            }
            if (!left)
                return std::nullopt;
            sched_cv.wait(lk);
        }
    };
    auto release = [&](size_t i) {
        {
            std::lock_guard lk(sched_mutex);
            reserved -= need[i];
        }
        sched_cv.notify_all();
    };
    auto worker = [&] {
        for (;;) {
            const auto next = take();
            if (!next)
                return;
            const size_t i = *next;
            struct Release {
                std::function<void()> f;
                ~Release() { f(); }
            } rel{[&, i] { release(i); }};
            const fs::path& src = shards[i];
            const fs::path o = obj / src.filename().replace_extension(".obj");
            if (fs::exists(o) && MTime(o) > MTime(src) && MTime(o) > header_time && fs::file_size(o, ec) > 0) {
                prog.done++;
                continue;
            }
            const fs::path shard_log = obj / src.filename().replace_extension(".log");
            fs::remove(shard_log, ec);
            const std::string base = Q(cl) + " " + kCompileFlags + " /I " + Q(p.sdk_dir / "include") + " /I " + Q(gen) +
                                     " /Fo" + Q(o) + " " + Q(src);
            int rc;
            if (need[i]) {
                rc = Run(W(base + " /d2OptimizeHugeFunctions"), &env, shard_log, BELOW_NORMAL_PRIORITY_CLASS, &prog.cancel);
                if (rc != 0 && !prog.cancel) { // out of memory (C1002 / C1060) or a crash: as before, unoptimised
                    std::ifstream in(shard_log);
                    const std::string text{std::istreambuf_iterator<char>(in), {}};
                    if (rc != 2 || text.find("C1002") != std::string::npos || text.find("C1060") != std::string::npos) {
                        note(fmt::format("{}: not enough memory to optimise it, compiling it as before", src.filename().string()));
                        rc = Run(W(base), &env, shard_log, BELOW_NORMAL_PRIORITY_CLASS, &prog.cancel);
                    }
                }
            } else {
                rc = Run(W(base), &env, shard_log, BELOW_NORMAL_PRIORITY_CLASS, &prog.cancel);
            }
            if (rc != 0 && rc != 2 && !prog.cancel) { // the optimizer crashed: this part without optimisation
                note(fmt::format("{}: compiler error {:#x}, retrying without optimisation", src.filename().string(), u32(rc)));
                std::string slow = base;
                slow.replace(slow.find("/O2 /Ob3"), 8, "/Od");
                rc = Run(W(slow), &env, shard_log, BELOW_NORMAL_PRIORITY_CLASS, &prog.cancel);
            }
            if (rc != 0) {
                if (!prog.cancel) {
                    std::lock_guard lk(broken_mutex);
                    broken_why = fmt::format("Compiling {} failed (code {}); see {}", src.filename().string(), rc,
                                             shard_log.string());
                    broken = true;
                }
                fs::remove(o, ec);
                return;
            }
            fs::remove(shard_log, ec);
            const int d = ++prog.done;
            if (need[i])
                note(fmt::format("optimising large functions ({}/{})", ++big_done, big_total));
            else
                note(fmt::format("compiled {} ({}/{})", src.filename().string(), d, shards.size()));
        }
    };
    std::vector<std::thread> pool;
    for (int k = 0; k < std::max(1, jobs); k++)
        pool.emplace_back(worker);
    for (auto& t : pool)
        t.join();
    if (prog.cancel)
        return fail("Cancelled (finished parts are kept; building again continues).");
    if (broken)
        return fail(broken_why);
    } // (not prebuilt)

    // 5. link mua3.exe
    stage("Linking mua3.exe");
    // the icon, as a resource (rc.exe from the Windows SDK that vcvars64 set up); without it the exe keeps the default
    fs::path res;
    if (!p.icon.empty() && fs::exists(p.icon)) {
        std::wstring sdk_bin;
        for (const wchar_t* e = env.data(); *e; e += wcslen(e) + 1)
            if (_wcsnicmp(e, L"WindowsSdkVerBinPath=", 21) == 0)
                sdk_bin = e + 21;
        const fs::path rc = fs::path(sdk_bin) / "x64" / "rc.exe";
        const fs::path rc_src = p.work_dir / "mua3_icon.rc";
        std::ofstream(rc_src) << "1 ICON \"" << fs::absolute(p.icon).generic_string() << "\"\n";
        res = p.work_dir / "mua3_icon.res";
        if (sdk_bin.empty() || !fs::exists(rc) ||
            Run(W(Q(rc) + " /nologo /fo " + Q(res) + " " + Q(rc_src)), &env, log, BELOW_NORMAL_PRIORITY_CLASS) != 0) {
            note("Could not compile the icon resource: mua3.exe keeps the default icon.");
            res.clear();
        }
    }
    const fs::path rsp = p.work_dir / "link.rsp";
    {
        std::ofstream r(rsp);
        r << "/NOLOGO /MACHINE:X64 " << (p.windowed ? "/SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup" : "/SUBSYSTEM:CONSOLE")
          << " /INCREMENTAL:NO /OPT:REF /OPT:NOICF\n";
        if (!res.empty())
            r << Q(res) << "\n";
        r << "/OUT:" << Q(p.work_dir / "mua3.exe") << "\n/LIBPATH:" << Q(p.sdk_dir / "lib") << "\n";
        for (const char* o : {"main.obj", "alloc_trace.obj"})
            r << Q(p.sdk_dir / "obj" / o) << "\n";
        for (auto& s : shards)
            r << Q(obj / s.filename().replace_extension(".obj")) << "\n";
        if (!p.prebuilt_code.empty())
            r << Q(p.prebuilt_code) << "\n";
        for (const char* l : kLibs)
            r << l << "\n";
    }
    if (const int rc = Run(W(Q(link) + " @" + Q(rsp)), &env, log, BELOW_NORMAL_PRIORITY_CLASS, &prog.cancel); rc != 0)
        return fail(fmt::format("Linking failed (code {}); see {}", rc, log.string()));
    fs::remove(p.out_exe, ec);
    fs::rename(p.work_dir / "mua3.exe", p.out_exe, ec);
    if (ec)
        return fail("Could not write " + p.out_exe.string() + " (is the game running?)");
    const double mins = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() / 60.0;
    stage(fmt::format("Done: {} built in {:.0f} minutes", p.out_exe.filename().string(), mins));
    prog.ok = true;
    prog.running = false;
    prog.finished = true;
    return true;
}

} // namespace builder
