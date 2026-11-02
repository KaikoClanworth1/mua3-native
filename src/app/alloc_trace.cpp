// MUA3_TRACE_VALLOC=<MB> (diagnostic): finds who commits large blocks of virtual memory. Every loaded module's
// import entries for VirtualAlloc / NtAllocateVirtualMemory are redirected to a hook that records the host stack
// of each commit of at least <MB> (default 4) MB; every 10 s the busiest stacks are logged with counts and bytes.
// Modules loaded later (graphics drivers) are patched by a periodic rescan.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <windows.h>
#include <dbghelp.h>
#include <psapi.h>

#include "common/common.h"

namespace {

using VirtualAllocFn = LPVOID(WINAPI*)(LPVOID, SIZE_T, DWORD, DWORD);
using NtAllocFn = LONG(NTAPI*)(HANDLE, PVOID*, ULONG_PTR, PSIZE_T, ULONG, ULONG);

VirtualAllocFn g_real_va = nullptr;
NtAllocFn g_real_nt = nullptr;
SIZE_T g_threshold = 4u << 20;
thread_local bool t_in_hook = false;

struct Site {
    u64 count = 0, bytes = 0;
    std::vector<void*> frames;
};
std::mutex g_mutex;
std::unordered_map<u64, Site> g_sites;

void Record(SIZE_T size) {
    if (t_in_hook)
        return;
    t_in_hook = true;
    void* frames[40];
    const USHORT n = RtlCaptureStackBackTrace(2, 40, frames, nullptr);
    u64 h = 0xcbf29ce484222325ull;
    for (USHORT i = 0; i < n; i++)
        h = (h ^ reinterpret_cast<u64>(frames[i])) * 0x100000001b3ull;
    {
        std::lock_guard lk(g_mutex);
        auto& s = g_sites[h];
        if (s.frames.empty())
            s.frames.assign(frames, frames + n);
        s.count++;
        s.bytes += size;
    }
    t_in_hook = false;
}

LPVOID WINAPI HookVirtualAlloc(LPVOID addr, SIZE_T size, DWORD type, DWORD protect) {
    if ((type & MEM_COMMIT) && size >= g_threshold)
        Record(size);
    const bool was = t_in_hook;
    t_in_hook = true; // the NtAllocateVirtualMemory it makes is the same commit
    LPVOID r = g_real_va(addr, size, type, protect);
    t_in_hook = was;
    return r;
}

LONG NTAPI HookNtAlloc(HANDLE proc, PVOID* base, ULONG_PTR zero_bits, PSIZE_T size, ULONG type, ULONG protect) {
    if ((type & MEM_COMMIT) && size && *size >= g_threshold && proc == GetCurrentProcess())
        Record(*size);
    return g_real_nt(proc, base, zero_bits, size, type, protect);
}

// Redirects `module`'s import of `name` (from any DLL) to `hook`. Returns the number of entries patched.
int PatchImports(HMODULE module, const char* name, void* hook) {
    ULONG size = 0;
    auto* desc = static_cast<PIMAGE_IMPORT_DESCRIPTOR>(
        ImageDirectoryEntryToDataEx(module, TRUE, IMAGE_DIRECTORY_ENTRY_IMPORT, &size, nullptr));
    if (!desc)
        return 0;
    auto* base = reinterpret_cast<u8*>(module);
    int patched = 0;
    for (; desc->Name; desc++) {
        if (!desc->OriginalFirstThunk)
            continue;
        auto* names = reinterpret_cast<PIMAGE_THUNK_DATA>(base + desc->OriginalFirstThunk);
        auto* iat = reinterpret_cast<PIMAGE_THUNK_DATA>(base + desc->FirstThunk);
        for (; names->u1.AddressOfData; names++, iat++) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal))
                continue;
            auto* ibn = reinterpret_cast<PIMAGE_IMPORT_BY_NAME>(base + names->u1.AddressOfData);
            if (std::strcmp(reinterpret_cast<const char*>(ibn->Name), name) != 0)
                continue;
            if (reinterpret_cast<void*>(iat->u1.Function) == hook)
                continue;
            DWORD old;
            if (VirtualProtect(&iat->u1.Function, sizeof(void*), PAGE_READWRITE, &old)) {
                iat->u1.Function = reinterpret_cast<ULONG_PTR>(hook);
                VirtualProtect(&iat->u1.Function, sizeof(void*), old, &old);
                patched++;
            }
        }
    }
    return patched;
}

int PatchAll() {
    HMODULE mods[1024];
    DWORD need = 0;
    if (!EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &need))
        return 0;
    int n = 0;
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    for (DWORD i = 0; i < need / sizeof(HMODULE); i++) {
        if (mods[i] == ntdll)
            continue;
        n += PatchImports(mods[i], "VirtualAlloc", reinterpret_cast<void*>(&HookVirtualAlloc));
        n += PatchImports(mods[i], "NtAllocateVirtualMemory", reinterpret_cast<void*>(&HookNtAlloc));
    }
    return n;
}

std::string Describe(void* pc) {
    const HANDLE proc = GetCurrentProcess();
    alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 256]{};
    auto* sym = reinterpret_cast<SYMBOL_INFO*>(buf);
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 255;
    DWORD64 disp = 0;
    if (SymFromAddr(proc, reinterpret_cast<DWORD64>(pc), &disp, sym))
        return fmt::format("{}+{:#x}", sym->Name, disp);
    HMODULE m = nullptr;
    char name[MAX_PATH]{};
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCSTR>(pc), &m) &&
        GetModuleFileNameA(m, name, MAX_PATH)) {
        const char* base = std::strrchr(name, '\\');
        return fmt::format("{}+{:#x}", base ? base + 1 : name, reinterpret_cast<u8*>(pc) - reinterpret_cast<u8*>(m));
    }
    return fmt::format("{:#x}", reinterpret_cast<u64>(pc));
}

void Report() {
    std::vector<std::pair<u64, Site>> sites;
    {
        std::lock_guard lk(g_mutex);
        for (auto& [h, s] : g_sites)
            sites.emplace_back(h, s);
    }
    std::sort(sites.begin(), sites.end(), [](auto& a, auto& b) { return a.second.bytes > b.second.bytes; });
    t_in_hook = true;
    static const bool sym_ready = SymInitialize(GetCurrentProcess(), nullptr, TRUE) != 0;
    (void)sym_ready;
    for (size_t i = 0; i < sites.size() && i < 6; i++) {
        std::string stack;
        for (size_t f = 0; f < sites[i].second.frames.size() && f < 24; f++)
            stack += "\n    " + Describe(sites[i].second.frames[f]);
        LOG_INFO("AllocTrace", "#{}: {} commits, {} MB total{}", i, sites[i].second.count, sites[i].second.bytes >> 20,
                 stack);
    }
    t_in_hook = false;
}

} // namespace

void StartAllocTrace() {
    const char* env = std::getenv("MUA3_TRACE_VALLOC");
    if (!env)
        return;
    if (const unsigned long mb = std::strtoul(env, nullptr, 10); mb)
        g_threshold = SIZE_T(mb) << 20;
    g_real_va = reinterpret_cast<VirtualAllocFn>(GetProcAddress(GetModuleHandleW(L"kernelbase.dll"), "VirtualAlloc"));
    g_real_nt = reinterpret_cast<NtAllocFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtAllocateVirtualMemory"));
    LOG_INFO("AllocTrace", "tracing commits >= {} MB: {} import entries patched", g_threshold >> 20, PatchAll());
    std::thread([] {
        for (int tick = 1;; tick++) {
            std::this_thread::sleep_for(std::chrono::seconds(2));
            if (const int n = PatchAll())
                LOG_INFO("AllocTrace", "{} more import entries patched", n);
            if (tick % 5 == 0)
                Report();
        }
    }).detach();
}
