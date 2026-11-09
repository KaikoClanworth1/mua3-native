#include "common/crash.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <ctime>
#include <fstream>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dbghelp.h>

#pragma comment(lib, "dbghelp.lib")

#ifndef MUA3_VERSION
#define MUA3_VERSION "dev"
#endif

namespace common {

namespace {

std::string Stamp() {
    const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm{};
    localtime_s(&tm, &now);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y%m%d_%H%M%S", &tm);
    return buf;
}

std::string ModuleOf(u64 addr, u64& offset) {
    HMODULE mod = nullptr;
    char name[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(addr), &mod)) {
        GetModuleFileNameA(mod, name, MAX_PATH);
        offset = addr - u64(mod);
    } else {
        offset = addr;
    }
    const char* base = std::max(std::strrchr(name, '\\'), std::strrchr(name, '/'));
    return base ? base + 1 : name;
}

const char* ExceptionName(DWORD code) {
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION: return "access violation";
    case EXCEPTION_STACK_OVERFLOW: return "stack overflow";
    case EXCEPTION_ILLEGAL_INSTRUCTION: return "illegal instruction";
    case EXCEPTION_INT_DIVIDE_BY_ZERO: return "integer divide by zero";
    case 0xE06D7363: return "C++ exception";
    case 0xC0000409: return "stack buffer overrun / fail fast";
    default: return "exception";
    }
}

std::string g_app;
std::function<void(const fs::path&)> g_after;

LONG WINAPI Filter(EXCEPTION_POINTERS* ep) {
    static std::atomic<bool> once{false};
    if (once.exchange(true))
        return EXCEPTION_CONTINUE_SEARCH;
    const fs::path s = WriteCrashReport(g_app, ep, "unhandled exception");
    if (g_after)
        g_after(s);
    return EXCEPTION_CONTINUE_SEARCH;
}

} // namespace

const char* Version() { return MUA3_VERSION; }

fs::path WriteCrashReport(const std::string& app, _EXCEPTION_POINTERS* ep, const std::string& reason) {
    std::error_code ec;
    fs::create_directories("crashes", ec);
    const std::string base = "crashes/" + app + "_" + Stamp();
    std::string text = fmt::format("{} crashed: {}\nversion {}\n", app, reason, Version());
    if (ep && ep->ExceptionRecord) {
        const auto* er = ep->ExceptionRecord;
        u64 off = 0;
        const std::string mod = ModuleOf(u64(er->ExceptionAddress), off);
        text += fmt::format("{} {:#x} at {}+{:#x}", ExceptionName(er->ExceptionCode), u32(er->ExceptionCode), mod, off);
        if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2)
            text += fmt::format(" ({} {:#x})", er->ExceptionInformation[0] == 1 ? "writing" : er->ExceptionInformation[0] == 8 ? "executing" : "reading",
                                er->ExceptionInformation[1]);
        text += "\nthread " + std::to_string(GetCurrentThreadId()) + "\nstack:\n";
        // The crashing thread's stack, as module+offset (names need the pdb, which the dump can be opened with).
        CONTEXT ctx = *ep->ContextRecord;
        for (int i = 0; i < 40 && ctx.Rip; i++) {
            u64 o = 0;
            const std::string m = ModuleOf(ctx.Rip, o);
            text += fmt::format("  #{:<2} {}+{:#x}\n", i, m, o);
            DWORD64 image = 0;
            RUNTIME_FUNCTION* fn = RtlLookupFunctionEntry(ctx.Rip, &image, nullptr);
            if (!fn) {
                if (!ctx.Rsp)
                    break;
                ctx.Rip = *reinterpret_cast<DWORD64*>(ctx.Rsp);
                ctx.Rsp += 8;
                continue;
            }
            void* handler = nullptr;
            DWORD64 frame = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, ctx.Rip, fn, &ctx, &handler, &frame, nullptr);
        }
    }
    LOG_ERROR("Crash", "{}", text); // (errors are written to the log file at once)
    // Minidump: thread stacks and module list, a few MB (no game memory).
    HANDLE f = CreateFileA((base + ".dmp").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION mei{GetCurrentThreadId(), ep, FALSE};
        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), f,
                          MINIDUMP_TYPE(MiniDumpNormal | MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules),
                          ep ? &mei : nullptr, nullptr, nullptr);
        CloseHandle(f);
    }
    std::ofstream(base + ".txt") << text;
    // Keep the newest ten crashes.
    std::vector<fs::path> old;
    for (auto& e : fs::directory_iterator("crashes", ec))
        if (e.path().extension() == ".txt" && e.path().filename().string().rfind(app + "_", 0) == 0)
            old.push_back(e.path());
    std::sort(old.begin(), old.end());
    for (size_t i = 0; i + 10 < old.size(); i++) {
        fs::remove(old[i], ec);
        fs::remove(fs::path(old[i]).replace_extension(".dmp"), ec);
    }
    return base + ".txt";
}

void InstallCrashHandler(const std::string& app, std::function<void(const fs::path&)> after) {
    g_app = app;
    g_after = std::move(after);
    SetUnhandledExceptionFilter(Filter);
    ULONG guarantee = 64 * 1024; // stack left for the filter after a stack overflow
    SetThreadStackGuarantee(&guarantee);
}

void RotateLog(const std::string& name, int keep) {
    std::error_code ec;
    const fs::path cur = name + ".log";
    if (fs::exists(cur, ec) && fs::file_size(cur, ec) > 0) {
        fs::create_directories("logs", ec);
        fs::rename(cur, fs::path("logs") / (name + "_" + Stamp() + ".log"), ec);
    }
    std::vector<fs::path> logs;
    for (auto& e : fs::directory_iterator("logs", ec))
        if (e.path().filename().string().rfind(name + "_", 0) == 0)
            logs.push_back(e.path());
    std::sort(logs.begin(), logs.end());
    for (size_t i = 0; i + keep < logs.size(); i++)
        fs::remove(logs[i], ec);
}

} // namespace common
