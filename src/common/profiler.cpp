#include "common/profiler.h"

#include <algorithm>
#include <map>
#include <set>
#include <thread>

#include <windows.h>
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")

namespace common {

void StartSamplingProfiler(void* thread_handle, const char* name, int seconds, int start_delay_seconds) {
    HANDLE dup = nullptr;
    DuplicateHandle(GetCurrentProcess(), (HANDLE)thread_handle, GetCurrentProcess(), &dup, 0, FALSE,
                    DUPLICATE_SAME_ACCESS);
    std::thread([dup, n = std::string(name), seconds, start_delay_seconds] {
        std::this_thread::sleep_for(std::chrono::seconds(start_delay_seconds));
        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
        static bool init = SymInitialize(GetCurrentProcess(), nullptr, TRUE) != FALSE;
        (void)init;
        std::map<std::string, int> self, inclusive;
        int samples = 0;
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
        while (std::chrono::steady_clock::now() < end) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            if (SuspendThread(dup) == DWORD(-1))
                break;
            // Only raw return addresses while the thread is suspended: it may hold the heap or dbghelp locks, so
            // nothing here may allocate (StackWalk64 does).
            CONTEXT ctx{};
            ctx.ContextFlags = CONTEXT_FULL;
            DWORD64 raw[12];
            int n = 0;
            if (GetThreadContext(dup, &ctx))
                for (; n < 12 && ctx.Rip; n++) {
                    raw[n] = ctx.Rip;
                    DWORD64 image = 0;
                    RUNTIME_FUNCTION* fn = RtlLookupFunctionEntry(ctx.Rip, &image, nullptr);
                    if (!fn) { // leaf: return address on top of the stack
                        ctx.Rip = *reinterpret_cast<DWORD64*>(ctx.Rsp);
                        ctx.Rsp += 8;
                        continue;
                    }
                    void* handler_data = nullptr;
                    DWORD64 frame = 0;
                    RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, ctx.Rip, fn, &ctx, &handler_data, &frame, nullptr);
                }
            ResumeThread(dup);
            std::vector<DWORD64> pcs(raw, raw + n);
            samples++;
            std::set<std::string> seen;
            for (size_t i = 0; i < pcs.size(); i++) {
                alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 256]{};
                auto* sym = reinterpret_cast<SYMBOL_INFO*>(buf);
                sym->SizeOfStruct = sizeof(SYMBOL_INFO);
                sym->MaxNameLen = 255;
                DWORD64 disp = 0;
                const std::string nm = SymFromAddr(GetCurrentProcess(), pcs[i], &disp, sym) ? sym->Name : "?";
                if (i == 0)
                    self[nm]++;
                if (seen.insert(nm).second)
                    inclusive[nm]++;
            }
        }
        auto top = [&](std::map<std::string, int>& m, const char* title) {
            std::vector<std::pair<int, std::string>> v;
            for (auto& [k, c] : m)
                v.emplace_back(c, k);
            std::sort(v.rbegin(), v.rend());
            std::string out = fmt::format("Profile {} ({} samples) {}:\n", n, samples, title);
            for (size_t i = 0; i < std::min<size_t>(v.size(), 600); i++)
                out += fmt::format("  {:5.1f}%  {}\n", 100.0 * v[i].first / std::max(samples, 1), v[i].second);
            LOG_INFO("Profiler", "{}", out);
        };
        top(self, "self");
        top(inclusive, "inclusive");
        CloseHandle(dup);
    }).detach();
}

} // namespace common
