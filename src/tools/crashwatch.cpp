// crashwatch: runs a program under a minimal debugger and prints a symbolized stack for crashes that in-process
// handlers cannot see (fail-fast / heap corruption, stack overflows, second-chance exceptions).
// usage: crashwatch <exe> [args...]   (stdout/stderr of the child are inherited)
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>

#include <windows.h>
#include <dbghelp.h>

static void PrintStack(HANDLE proc, HANDLE thread) {
    CONTEXT ctx{};
    ctx.ContextFlags = CONTEXT_FULL;
    if (!GetThreadContext(thread, &ctx)) {
        std::printf("  (GetThreadContext failed)\n");
        return;
    }
    STACKFRAME64 f{};
    f.AddrPC = {ctx.Rip, 0, AddrModeFlat};
    f.AddrFrame = {ctx.Rbp, 0, AddrModeFlat};
    f.AddrStack = {ctx.Rsp, 0, AddrModeFlat};
    for (int i = 0; i < 40; i++) {
        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, proc, thread, &f, &ctx, nullptr, SymFunctionTableAccess64,
                         SymGetModuleBase64, nullptr) ||
            !f.AddrPC.Offset)
            break;
        alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 256]{};
        auto* sym = reinterpret_cast<SYMBOL_INFO*>(buf);
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 255;
        DWORD64 disp = 0;
        IMAGEHLP_LINE64 line{sizeof(line)};
        DWORD ldisp = 0;
        const bool has_sym = SymFromAddr(proc, f.AddrPC.Offset, &disp, sym);
        const bool has_line = SymGetLineFromAddr64(proc, f.AddrPC.Offset, &ldisp, &line);
        std::printf("  #%d %016llx %s+0x%llx %s:%lu\n", i, f.AddrPC.Offset, has_sym ? sym->Name : "?", disp,
                    has_line ? line.FileName : "", has_line ? line.LineNumber : 0);
    }
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: crashwatch <exe> [args...]\n");
        return 2;
    }
    std::string cmd;
    for (int i = 1; i < argc; i++)
        cmd += std::string(i > 1 ? " \"" : "\"") + argv[i] + "\"";
    STARTUPINFOA si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, TRUE, DEBUG_ONLY_THIS_PROCESS, nullptr, nullptr, &si, &pi)) {
        std::printf("crashwatch: CreateProcess failed %lu\n", GetLastError());
        return 1;
    }
    std::unordered_map<DWORD, HANDLE> threads;
    threads[pi.dwThreadId] = pi.hThread;
    bool sym_ready = false;
    DWORD exit_code = 0;
    DWORD64 main_base = 0;
    for (;;) {
        DEBUG_EVENT ev{};
        if (!WaitForDebugEvent(&ev, INFINITE))
            break;
        DWORD status = DBG_CONTINUE;
        switch (ev.dwDebugEventCode) {
        case CREATE_PROCESS_DEBUG_EVENT:
            SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
            sym_ready = SymInitialize(pi.hProcess, nullptr, FALSE);
            if (sym_ready && ev.u.CreateProcessInfo.hFile) { // the main image: invading at this point misses it
                char path[MAX_PATH * 2]{};
                const DWORD n = GetFinalPathNameByHandleA(ev.u.CreateProcessInfo.hFile, path, sizeof(path), FILE_NAME_NORMALIZED);
                const char* name = n > 4 && std::strncmp(path, "\\\\?\\", 4) == 0 ? path + 4 : path;
                const DWORD64 base = DWORD64(ev.u.CreateProcessInfo.lpBaseOfImage);
                std::printf("crashwatch: %s at %016llx\n", name, base);
                SymLoadModuleEx(pi.hProcess, nullptr, name, nullptr, base, 0, nullptr, 0);
                IMAGEHLP_MODULE64 mi{};
                mi.SizeOfStruct = sizeof(mi);
                if (SymGetModuleInfo64(pi.hProcess, base, &mi))
                    std::printf("crashwatch: symbols %s (%s)\n", mi.SymType == SymPdb ? "from pdb" : mi.SymType == SymNone ? "NONE" : "other",
                                mi.LoadedPdbName);
                main_base = base;
            }
            if (ev.u.CreateProcessInfo.hFile)
                CloseHandle(ev.u.CreateProcessInfo.hFile);
            break;
        case CREATE_THREAD_DEBUG_EVENT:
            threads[ev.dwThreadId] = ev.u.CreateThread.hThread;
            break;
        case EXIT_THREAD_DEBUG_EVENT:
            threads.erase(ev.dwThreadId);
            break;
        case LOAD_DLL_DEBUG_EVENT:
            if (sym_ready && ev.u.LoadDll.hFile) {
                char path[MAX_PATH * 2]{};
                const DWORD n = GetFinalPathNameByHandleA(ev.u.LoadDll.hFile, path, sizeof(path), FILE_NAME_NORMALIZED);
                const char* name = n > 4 && std::strncmp(path, "\\\\?\\", 4) == 0 ? path + 4 : path;
                SymLoadModuleEx(pi.hProcess, nullptr, name, nullptr, DWORD64(ev.u.LoadDll.lpBaseOfDll), 0, nullptr, 0);
            }
            if (ev.u.LoadDll.hFile)
                CloseHandle(ev.u.LoadDll.hFile);
            break;
        case EXCEPTION_DEBUG_EVENT: {
            const auto& er = ev.u.Exception.ExceptionRecord;
            const bool first = ev.u.Exception.dwFirstChance != 0;
            const DWORD code = er.ExceptionCode;
            // C++ throws (0xE06D7363) are reported at first chance: the stack is still intact at the throw site.
            const bool interesting = !first || code == STATUS_STACK_BUFFER_OVERRUN || code == STATUS_HEAP_CORRUPTION ||
                                     code == STATUS_STACK_OVERFLOW || code == 0xC0000374 || code == 0xE06D7363;
            if (code == EXCEPTION_BREAKPOINT && first) {
                status = DBG_CONTINUE; // loader breakpoint
                break;
            }
            status = DBG_EXCEPTION_NOT_HANDLED;
            if (interesting) {
                std::printf("crashwatch: %s exception %08lx at %p (thread %lu, info %llx %llx)\n",
                            first ? "first-chance" : "SECOND-CHANCE", code, er.ExceptionAddress, ev.dwThreadId,
                            er.NumberParameters > 0 ? (unsigned long long)(er.ExceptionInformation[0]) : 0ull,
                            er.NumberParameters > 1 ? (unsigned long long)(er.ExceptionInformation[1]) : 0ull);
                if (threads.count(ev.dwThreadId))
                    PrintStack(pi.hProcess, threads[ev.dwThreadId]);
                std::fflush(stdout);
            }
            break;
        }
        case EXIT_PROCESS_DEBUG_EVENT:
            exit_code = ev.u.ExitProcess.dwExitCode;
            std::printf("crashwatch: process exited with %08lx\n", exit_code);
            std::fflush(stdout);
            ContinueDebugEvent(ev.dwProcessId, ev.dwThreadId, DBG_CONTINUE);
            return int(exit_code);
        default:
            break;
        }
        ContinueDebugEvent(ev.dwProcessId, ev.dwThreadId, status);
    }
    return int(exit_code);
}
