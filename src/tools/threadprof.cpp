// threadprof: sampling profiler for a running process, from outside it (needs no admin rights and no game rebuild).
// Picks the threads using at least <min cpu %> of a core, then repeatedly suspends one, walks its stack and resumes it.
// Waiting shows up as samples in the wait function, so a thread's samples are wall time and "cpu %" says how much of
// that time it really ran.
// Usage: threadprof <pid> <seconds> <output file> [min cpu %, default 5]
// Output: per-thread CPU, then "thread;outermost;...;innermost count" lines (collapsed stacks).
#include <windows.h>
#include <dbghelp.h>
#include <timeapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#pragma comment(lib, "dbghelp.lib")
#pragma comment(lib, "winmm.lib")

namespace {

struct Target {
    DWORD id = 0;
    HANDLE handle = nullptr;
    std::string name;
    double cpu_start = 0;
    double cpu_percent = 0;
    int samples = 0;
    std::map<std::vector<DWORD64>, int> stacks;
};

double CpuSeconds(HANDLE th) {
    FILETIME c, e, k, u;
    if (!GetThreadTimes(th, &c, &e, &k, &u))
        return 0;
    const auto v = [](const FILETIME& f) { return double((static_cast<unsigned long long>(f.dwHighDateTime) << 32) | f.dwLowDateTime) / 1e7; };
    return v(k) + v(u);
}

std::string ThreadName(HANDLE th, DWORD id) {
    PWSTR d = nullptr;
    std::string out;
    if (SUCCEEDED(GetThreadDescription(th, &d)) && d) {
        for (PWSTR p = d; *p; p++)
            out += char(*p < 128 ? *p : '?');
        LocalFree(d);
    }
    return (out.empty() ? "unnamed" : out) + "#" + std::to_string(id);
}

std::vector<Target> OpenThreads(DWORD pid) {
    std::vector<Target> all;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te{sizeof(te)};
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid)
            continue;
        HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
        if (!th)
            continue;
        Target t;
        t.id = te.th32ThreadID;
        t.handle = th;
        t.name = ThreadName(th, t.id);
        t.cpu_start = CpuSeconds(th);
        all.push_back(std::move(t));
    }
    CloseHandle(snap);
    return all;
}

// Only the walk happens while the thread is stopped; names are looked up at the end.
void Sample(HANDLE proc, Target& t) {
    if (SuspendThread(t.handle) == DWORD(-1))
        return;
    CONTEXT ctx{};
    ctx.ContextFlags = CONTEXT_FULL;
    std::vector<DWORD64> pcs;
    if (GetThreadContext(t.handle, &ctx)) {
        STACKFRAME64 f{};
        f.AddrPC = {ctx.Rip, 0, AddrModeFlat};
        f.AddrFrame = {ctx.Rbp, 0, AddrModeFlat};
        f.AddrStack = {ctx.Rsp, 0, AddrModeFlat};
        for (int i = 0; i < 40; i++) {
            if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, proc, t.handle, &f, &ctx, nullptr, SymFunctionTableAccess64,
                             SymGetModuleBase64, nullptr) ||
                !f.AddrPC.Offset)
                break;
            pcs.push_back(f.AddrPC.Offset);
        }
    }
    ResumeThread(t.handle);
    t.samples++;
    t.stacks[std::move(pcs)]++;
}

std::string Name(HANDLE proc, DWORD64 pc, std::unordered_map<DWORD64, std::string>& cache) {
    auto it = cache.find(pc);
    if (it != cache.end())
        return it->second;
    char buf[sizeof(SYMBOL_INFO) + 512]{};
    auto* sym = reinterpret_cast<SYMBOL_INFO*>(buf);
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 511;
    DWORD64 disp = 0;
    std::string out;
    if (SymFromAddr(proc, pc, &disp, sym)) {
        out = sym->Name;
    } else {
        IMAGEHLP_MODULE64 mod{};
        mod.SizeOfStruct = sizeof(mod);
        char text[128];
        if (SymGetModuleInfo64(proc, pc, &mod))
            std::snprintf(text, sizeof(text), "%s!0x%llx", mod.ModuleName, (unsigned long long)(pc - mod.BaseOfImage));
        else
            std::snprintf(text, sizeof(text), "0x%llx", (unsigned long long)pc);
        out = text;
    }
    std::replace(out.begin(), out.end(), ';', ':');
    std::replace(out.begin(), out.end(), ' ', '_');
    return cache[pc] = out;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::printf("usage: threadprof <pid> <seconds> <output file> [min cpu %%]\n");
        return 1;
    }
    const DWORD pid = strtoul(argv[1], nullptr, 10);
    const int seconds = std::atoi(argv[2]);
    const double min_cpu = argc > 4 ? std::atof(argv[4]) : 5.0;
    HANDLE proc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!proc) {
        std::printf("cannot open process %lu (%lu)\n", pid, GetLastError());
        return 1;
    }
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    // The exe names its pdb without a folder, so look next to the exe.
    char image[MAX_PATH]{};
    DWORD image_size = MAX_PATH;
    QueryFullProcessImageNameA(proc, 0, image, &image_size);
    if (char* slash = std::strrchr(image, '\\'))
        *slash = 0;
    SymInitialize(proc, image, TRUE);

    // Pick the busy threads.
    std::vector<Target> all = OpenThreads(pid);
    Sleep(3000);
    std::vector<Target> targets;
    for (Target& t : all) {
        const double percent = (CpuSeconds(t.handle) - t.cpu_start) / 3.0 * 100.0;
        if (percent >= min_cpu)
            targets.push_back(t);
        else
            CloseHandle(t.handle);
    }
    if (targets.empty()) {
        std::printf("no busy threads\n");
        return 1;
    }

    // The first walks load the symbols, which is slow: walk once and throw the result away.
    Sample(proc, targets[0]);
    for (Target& t : targets) {
        t.stacks.clear();
        t.samples = 0;
        t.cpu_start = CpuSeconds(t.handle);
    }

    LARGE_INTEGER freq, start, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);
    double elapsed = 0;
    timeBeginPeriod(1);
    while (elapsed < seconds) {
        for (Target& t : targets)
            Sample(proc, t);
        Sleep(2);
        QueryPerformanceCounter(&now);
        elapsed = double(now.QuadPart - start.QuadPart) / double(freq.QuadPart);
    }
    timeEndPeriod(1);
    for (Target& t : targets)
        t.cpu_percent = (CpuSeconds(t.handle) - t.cpu_start) / elapsed * 100.0;

    FILE* out = std::fopen(argv[3], "w");
    if (!out) {
        std::printf("cannot write %s\n", argv[3]);
        return 1;
    }
    std::sort(targets.begin(), targets.end(), [](const Target& a, const Target& b) { return a.cpu_percent > b.cpu_percent; });
    std::unordered_map<DWORD64, std::string> names;
    for (const Target& t : targets)
        std::fprintf(out, "# %s cpu %.1f%% samples %d over %.1f s\n", t.name.c_str(), t.cpu_percent, t.samples, elapsed);
    for (const Target& t : targets)
        for (const auto& [pcs, count] : t.stacks) {
            std::string line = t.name;
            for (auto it = pcs.rbegin(); it != pcs.rend(); ++it)
                line += ";" + Name(proc, *it, names);
            std::fprintf(out, "%s %d\n", line.c_str(), count);
        }
    std::fclose(out);
    SymCleanup(proc);
    std::printf("wrote %s: %zu threads\n", argv[3], targets.size());
    return 0;
}
