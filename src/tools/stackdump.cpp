// stackdump: print the native call stacks of every thread in a running process (debugging hangs without a debugger).
// Usage: stackdump <pid | image.exe> [filter-substring]
#include <windows.h>
#include <dbghelp.h>
#include <tlhelp32.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#pragma comment(lib, "dbghelp.lib")

static DWORD FindPid(const char* arg) {
    if (const DWORD pid = strtoul(arg, nullptr, 10))
        return pid;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32 pe{sizeof(pe)};
    DWORD pid = 0;
    for (BOOL ok = Process32First(snap, &pe); ok; ok = Process32Next(snap, &pe))
        if (_stricmp(pe.szExeFile, arg) == 0)
            pid = pe.th32ProcessID;
    CloseHandle(snap);
    return pid;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: stackdump <pid|image.exe> [filter]\n");
        return 1;
    }
    const DWORD pid = FindPid(argv[1]);
    const char* filter = argc > 2 ? argv[2] : nullptr;
    HANDLE proc = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
    if (!proc) {
        std::printf("cannot open process %s (%lu)\n", argv[1], GetLastError());
        return 1;
    }
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    SymInitialize(proc, nullptr, TRUE);

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te{sizeof(te)};
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid)
            continue;
        HANDLE th = OpenThread(THREAD_ALL_ACCESS, FALSE, te.th32ThreadID);
        if (!th)
            continue;
        std::wstring desc;
        PWSTR d = nullptr;
        if (SUCCEEDED(GetThreadDescription(th, &d)) && d) {
            desc = d;
            LocalFree(d);
        }
        SuspendThread(th);
        CONTEXT ctx{};
        ctx.ContextFlags = CONTEXT_FULL;
        std::vector<std::string> frames;
        if (GetThreadContext(th, &ctx)) {
            STACKFRAME64 f{};
            f.AddrPC = {ctx.Rip, 0, AddrModeFlat};
            f.AddrFrame = {ctx.Rbp, 0, AddrModeFlat};
            f.AddrStack = {ctx.Rsp, 0, AddrModeFlat};
            for (int i = 0; i < 48; i++) {
                if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, proc, th, &f, &ctx, nullptr, SymFunctionTableAccess64,
                                 SymGetModuleBase64, nullptr) ||
                    !f.AddrPC.Offset)
                    break;
                char buf[sizeof(SYMBOL_INFO) + 512]{};
                auto* sym = reinterpret_cast<SYMBOL_INFO*>(buf);
                sym->SizeOfStruct = sizeof(SYMBOL_INFO);
                sym->MaxNameLen = 511;
                DWORD64 disp = 0;
                char line[700];
                if (SymFromAddr(proc, f.AddrPC.Offset, &disp, sym))
                    std::snprintf(line, sizeof(line), "%s+0x%llx", sym->Name, (unsigned long long)disp);
                else
                    std::snprintf(line, sizeof(line), "0x%llx", (unsigned long long)f.AddrPC.Offset);
                frames.emplace_back(line);
            }
        }
        ResumeThread(th);
        CloseHandle(th);
        bool show = !filter;
        for (auto& s : frames)
            if (filter && s.find(filter) != std::string::npos)
                show = true;
        if (!show)
            continue;
        std::printf("--- thread %lu %ls\n", te.th32ThreadID, desc.c_str());
        for (auto& s : frames)
            std::printf("    %s\n", s.c_str());
    }
    CloseHandle(snap);
    SymCleanup(proc);
    return 0;
}
