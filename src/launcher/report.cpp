#include "launcher/report.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <ctime>
#include <fstream>
#include <intrin.h>

#include <windows.h>
#include <dxgi.h>

#include "common/crash.h"

#pragma comment(lib, "dxgi.lib")

namespace report {

namespace {

std::string Stamp(const char* f) {
    const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm{};
    localtime_s(&tm, &now);
    char buf[64];
    std::strftime(buf, sizeof(buf), f, &tm);
    return buf;
}

u64 FileTime(const fs::path& p) {
    std::error_code ec;
    return u64(fs::last_write_time(p, ec).time_since_epoch().count());
}

// ---- a minimal zip writer (stored entries: logs and dumps are small, and every unzip tool reads them)
class Zip {
public:
    explicit Zip(const fs::path& p) : f_(p, std::ios::binary) {}
    bool ok() const { return bool(f_); }
    void Add(const std::string& name, const std::vector<u8>& data) {
        const u32 crc = Crc(data);
        const u32 offset = u32(f_.tellp());
        u16 time = 0, date = 0;
        DosNow(time, date);
        // local header
        U32(0x04034b50); U16(20); U16(0x0800 /* UTF-8 names */); U16(0); U16(time); U16(date);
        U32(crc); U32(u32(data.size())); U32(u32(data.size())); U16(u16(name.size())); U16(0);
        f_.write(name.data(), std::streamsize(name.size()));
        f_.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
        entries_.push_back({name, crc, u32(data.size()), offset, time, date});
    }
    void Finish() {
        const u32 dir = u32(f_.tellp());
        for (auto& e : entries_) {
            U32(0x02014b50); U16(20); U16(20); U16(0x0800); U16(0); U16(e.time); U16(e.date);
            U32(e.crc); U32(e.size); U32(e.size); U16(u16(e.name.size())); U16(0); U16(0); U16(0); U16(0); U32(0); U32(e.offset);
            f_.write(e.name.data(), std::streamsize(e.name.size()));
        }
        const u32 len = u32(f_.tellp()) - dir;
        U32(0x06054b50); U16(0); U16(0); U16(u16(entries_.size())); U16(u16(entries_.size())); U32(len); U32(dir); U16(0);
    }

private:
    struct Entry {
        std::string name;
        u32 crc, size, offset;
        u16 time, date;
    };
    void U16(u16 v) { f_.write(reinterpret_cast<const char*>(&v), 2); }
    void U32(u32 v) { f_.write(reinterpret_cast<const char*>(&v), 4); }
    static u32 Crc(const std::vector<u8>& d) {
        static const std::array<u32, 256> table = [] {
            std::array<u32, 256> t{};
            for (u32 i = 0; i < 256; i++) {
                u32 c = i;
                for (int k = 0; k < 8; k++)
                    c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
                t[i] = c;
            }
            return t;
        }();
        u32 c = 0xFFFFFFFFu;
        for (u8 b : d)
            c = table[(c ^ b) & 0xFF] ^ (c >> 8);
        return c ^ 0xFFFFFFFFu;
    }
    static void DosNow(u16& time, u16& date) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        time = u16((st.wHour << 11) | (st.wMinute << 5) | (st.wSecond / 2));
        date = u16(((st.wYear - 1980) << 9) | (st.wMonth << 5) | st.wDay);
    }
    std::ofstream f_;
    std::vector<Entry> entries_;
};

// Reads a file another process may still be writing (the game's log).
std::vector<u8> ReadShared(const fs::path& p, u64 limit) {
    std::vector<u8> out;
    HANDLE h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return out;
    LARGE_INTEGER size{};
    GetFileSizeEx(h, &size);
    u64 n = u64(size.QuadPart), start = 0;
    if (n > limit) { // a huge log: its end matters most
        start = n - limit;
        n = limit;
        LARGE_INTEGER pos{};
        pos.QuadPart = LONGLONG(start);
        SetFilePointerEx(h, pos, nullptr, FILE_BEGIN);
    }
    out.resize(size_t(n));
    DWORD got = 0;
    size_t at = 0;
    while (at < out.size() && ReadFile(h, out.data() + at, DWORD(std::min<size_t>(out.size() - at, 1u << 24)), &got, nullptr) && got)
        at += got;
    out.resize(at);
    CloseHandle(h);
    return out;
}

} // namespace

std::optional<Crash> NewestCrash() {
    std::error_code ec;
    std::optional<Crash> best;
    for (auto& e : fs::directory_iterator("crashes", ec)) {
        if (e.path().extension() != ".txt")
            continue;
        const u64 t = FileTime(e.path());
        if (best && t <= best->time)
            continue;
        Crash c;
        c.summary = e.path();
        c.time = t;
        std::ifstream f(e.path());
        std::string l1, l2, l3;
        std::getline(f, l1);
        std::getline(f, l2);
        std::getline(f, l3);
        c.headline = l1 + (l3.empty() ? "" : " / " + l3);
        best = c;
    }
    return best;
}

std::string SystemInfo() {
    std::string s;
    // Windows (RtlGetVersion: the real version, not a compatibility shim's)
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    OSVERSIONINFOW v{sizeof(v)};
    if (auto fn = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion")))
        fn(&v);
    s += fmt::format("Windows {}.{} build {}\n", v.dwMajorVersion, v.dwMinorVersion, v.dwBuildNumber);
    // CPU
    int r[4]{};
    char brand[49]{};
    for (int i = 0; i < 3; i++) {
        __cpuid(r, int(0x80000002u + i));
        std::memcpy(brand + i * 16, r, 16);
    }
    SYSTEM_INFO si;
    GetNativeSystemInfo(&si);
    s += fmt::format("CPU {} ({} logical processors)\n", brand, si.dwNumberOfProcessors);
    MEMORYSTATUSEX ms{sizeof(ms)};
    GlobalMemoryStatusEx(&ms);
    s += fmt::format("Memory {:.1f} GB ({:.1f} GB free)\n", ms.ullTotalPhys / 1073741824.0, ms.ullAvailPhys / 1073741824.0);
    // GPUs and driver versions
    IDXGIFactory1* factory = nullptr;
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        IDXGIAdapter1* a = nullptr;
        std::vector<LUID> seen;
        for (UINT i = 0; factory->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; i++) {
            DXGI_ADAPTER_DESC1 d{};
            a->GetDesc1(&d);
            const bool dup = std::any_of(seen.begin(), seen.end(), [&](const LUID& l) {
                return l.LowPart == d.AdapterLuid.LowPart && l.HighPart == d.AdapterLuid.HighPart;
            });
            seen.push_back(d.AdapterLuid);
            if (dup) { // (the same adapter is listed once per output on some systems)
                a->Release();
                continue;
            }
            LARGE_INTEGER drv{};
            a->CheckInterfaceSupport(__uuidof(IDXGIDevice), &drv);
            char name[128]{};
            WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, name, sizeof(name), nullptr, nullptr);
            const std::string line = fmt::format("GPU {} ({} MB), driver {}.{}.{}.{}\n", name, d.DedicatedVideoMemory >> 20,
                                                 HIWORD(drv.HighPart), LOWORD(drv.HighPart), HIWORD(drv.LowPart),
                                                 LOWORD(drv.LowPart));
            if (!(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) && s.find(line) == std::string::npos) // (one line per card)
                s += line;
            a->Release();
        }
        factory->Release();
    }
    return s;
}

std::vector<Item> Collect(const fs::path& game_dir, const fs::path& build_dir, const fs::path& save_dir) {
    std::vector<Item> items;
    std::error_code ec;
    auto add = [&](const fs::path& p, const std::string& name) {
        if (fs::is_regular_file(p, ec))
            items.push_back({p, name, fs::file_size(p, ec)});
    };
    add("mua3.log", "logs/mua3.log");
    add("launcher.log", "logs/launcher.log");
    std::vector<fs::path> logs, crashes;
    for (auto& e : fs::directory_iterator("logs", ec))
        logs.push_back(e.path());
    std::sort(logs.rbegin(), logs.rend()); // newest first (time-stamped names)
    for (size_t i = 0; i < logs.size() && i < 4; i++)
        add(logs[i], "logs/" + logs[i].filename().string());
    for (auto& e : fs::directory_iterator("crashes", ec))
        if (e.path().extension() == ".txt")
            crashes.push_back(e.path());
    std::sort(crashes.begin(), crashes.end(), [](auto& a, auto& b) { return FileTime(a) > FileTime(b); });
    for (size_t i = 0; i < crashes.size() && i < 3; i++) {
        add(crashes[i], "crashes/" + crashes[i].filename().string());
        const fs::path dmp = fs::path(crashes[i]).replace_extension(".dmp");
        add(dmp, "crashes/" + dmp.filename().string());
    }
    add("mua3.ini", "settings/mua3.ini");
    add("launcher.ini", "settings/launcher.ini");
    add(game_dir / "install.json", "settings/install.json");
    add(build_dir / "build.log", "build/build.log");
    // the save (a few hundred KB): lets the problem be reached again from the same point
    if (!save_dir.empty() && fs::is_directory(save_dir, ec))
        for (auto it = fs::recursive_directory_iterator(save_dir, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
            if (it->is_regular_file(ec) && it->file_size(ec) < (64ull << 20))
                add(it->path(), "save/" + it->path().lexically_relative(save_dir).generic_string());
    return items;
}

Result Create(const std::string& about, const std::vector<std::string>& extra, const fs::path& game_dir,
              const fs::path& build_dir, const fs::path& save_dir) {
    Result r;
    std::error_code ec;
    fs::create_directories("reports", ec);
    r.zip = fs::absolute(fs::path("reports") / ("MUA3_report_" + Stamp("%Y%m%d_%H%M%S") + ".zip"), ec);
    Zip zip(r.zip);
    if (!zip.ok()) {
        r.error = "Could not create " + r.zip.string();
        return r;
    }
    std::string text = "MUA3 Native problem report\n";
    text += "Made " + Stamp("%Y-%m-%d %H:%M:%S") + ", launcher " + common::Version() + "\n\n";
    text += "What happened (from the player):\n" + (about.empty() ? std::string("(nothing written)") : about) + "\n\n";
    if (auto c = NewestCrash())
        text += "Newest crash: " + c->headline + " (" + c->summary.filename().string() + ")\n\n";
    text += "System:\n" + SystemInfo() + "\n";
    for (auto& l : extra)
        text += l + "\n";
    zip.Add("report.txt", std::vector<u8>(text.begin(), text.end()));
    for (const Item& it : Collect(game_dir, build_dir, save_dir))
        zip.Add(it.name, ReadShared(it.path, 64ull << 20));
    zip.Finish();
    r.ok = zip.ok();
    if (!r.ok)
        r.error = "Writing " + r.zip.string() + " failed";
    LOG_INFO("Launcher", "problem report written: {}", r.zip.string());
    return r;
}

} // namespace report
