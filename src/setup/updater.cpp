// The launcher's updater (see updater.h).
#include "setup/updater.h"

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <vector>

#include <windows.h>
#include <shellapi.h>

#include "setup/install_ops.h"

namespace updater {
namespace {

constexpr const char* kApi = "https://api.github.com/repos/KaikoClanworth1/mua3-native/releases/latest";
constexpr const char* kPackageSuffix = "-Windows-x64.zip"; // (scripts/make_release.ps1 names it so)

std::vector<int> Numbers(const std::string& v) {
    std::vector<int> n;
    size_t i = 0;
    while (i < v.size() && (v[i] == 'v' || v[i] == 'V' || v[i] == ' '))
        i++;
    while (i < v.size() && std::isdigit(u8(v[i]))) {
        int x = 0;
        while (i < v.size() && std::isdigit(u8(v[i])))
            x = x * 10 + (v[i++] - '0');
        n.push_back(x);
        if (i < v.size() && v[i] == '.')
            i++;
        else
            break;
    }
    return n;
}

fs::path SystemExe(const wchar_t* name) {
    wchar_t sys[MAX_PATH];
    GetSystemDirectoryW(sys, MAX_PATH);
    return fs::path(sys) / name;
}

// Runs a console program hidden; `tick` is called about 5 times a second while it runs. Returns its exit code (-1:
// it did not start).
template <class F>
int RunHidden(const fs::path& exe, const std::wstring& args, const fs::path& dir, F&& tick) {
    std::wstring cmd = L"\"" + exe.wstring() + L"\" " + args;
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                        dir.empty() ? nullptr : dir.c_str(), &si, &pi))
        return -1;
    CloseHandle(pi.hThread);
    while (WaitForSingleObject(pi.hProcess, 200) == WAIT_TIMEOUT)
        tick();
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    return int(code);
}

// The string value after "key": at or after `from` (empty when there is none).
std::string JsonString(const std::string& j, const std::string& key, size_t from, size_t* at = nullptr) {
    const size_t k = j.find("\"" + key + "\"", from);
    if (k == std::string::npos)
        return {};
    size_t q = j.find(':', k);
    q = q == std::string::npos ? q : j.find('"', q);
    if (q == std::string::npos)
        return {};
    std::string s;
    size_t i = q + 1;
    for (; i < j.size() && j[i] != '"'; i++) {
        if (j[i] == '\\' && i + 1 < j.size())
            i++;
        s += j[i];
    }
    if (at)
        *at = k;
    return s;
}

fs::path PackagesDir() { return setup::LocalDataDir() / "packages"; }

} // namespace

bool Newer(const std::string& a, const std::string& b) {
    const std::vector<int> x = Numbers(a), y = Numbers(b);
    if (x.empty() || y.empty())
        return false;
    for (size_t i = 0; i < std::max(x.size(), y.size()); i++) {
        const int p = i < x.size() ? x[i] : 0, q = i < y.size() ? y[i] : 0;
        if (p != q)
            return p > q;
    }
    return false;
}

std::optional<Release> Latest(std::string& error) {
    std::string json;
    const char* api = std::getenv("MUA3_UPDATE_API");
    std::error_code ec;
    if (api && fs::exists(fs::u8path(api), ec)) { // (developer test: a saved answer)
        std::ifstream f(fs::u8path(api), std::ios::binary);
        json.assign(std::istreambuf_iterator<char>(f), {});
    } else {
        fs::create_directories(PackagesDir(), ec);
        const fs::path out = PackagesDir() / "latest.json";
        fs::remove(out, ec);
        const int code = RunHidden(SystemExe(L"curl.exe"),
                                   L"-L -f -s --max-time 20 -A MUA3Native-launcher -H \"Accept: application/vnd.github+json\" -o \"" +
                                       out.wstring() + L"\" " + fs::u8path(api ? api : kApi).wstring(),
                                   {}, [] {});
        if (code != 0) {
            error = code == -1 ? "curl.exe is missing" : fmt::format("no answer from GitHub (curl {})", code);
            return std::nullopt;
        }
        std::ifstream f(out, std::ios::binary);
        json.assign(std::istreambuf_iterator<char>(f), {});
    }
    Release r;
    r.version = JsonString(json, "tag_name", 0);
    // the Windows package among the assets; its "size" comes before its URL in the same asset
    for (size_t at = 0, pos = 0;; pos = at + 1) {
        const std::string url = JsonString(json, "browser_download_url", pos, &at);
        if (url.empty())
            break;
        if (url.size() > std::strlen(kPackageSuffix) &&
            url.compare(url.size() - std::strlen(kPackageSuffix), std::string::npos, kPackageSuffix) == 0) {
            r.url = url;
            const size_t s = json.rfind("\"size\"", at);
            if (s != std::string::npos)
                if (const size_t c = json.find(':', s); c != std::string::npos)
                    r.size = std::strtoull(json.c_str() + c + 1, nullptr, 10);
            break;
        }
    }
    const bool test_file = api && r.url.rfind("file:///", 0) == 0; // (developer test: a local package)
    if (r.version.empty() || r.url.empty() || (r.url.rfind("https://", 0) != 0 && !test_file)) {
        error = "GitHub's answer has no Windows package";
        return std::nullopt;
    }
    return r;
}

void Updater::Join() {
    if (thread.joinable())
        thread.join();
}

Updater::~Updater() { Join(); }

void Updater::Check(const std::string& current_in) {
    if (state != State::Idle && state != State::UpToDate && state != State::Failed)
        return;
    std::string current = current_in;
    if (const char* c = std::getenv("MUA3_UPDATE_CURRENT"))
        current = c;
    if (Numbers(current).empty()) { // a developer build ("dev"): nothing to compare with
        LOG_INFO("Update", "no update check for version {}", current);
        return;
    }
    Join();
    state = State::Checking;
    thread = std::thread([this, current] {
        std::string err;
        const auto r = Latest(err);
        if (!r) {
            LOG_WARN("Update", "update check: {}", err);
            std::lock_guard lk(mutex);
            error = err;
            state = State::Failed;
            return;
        }
        const bool newer = Newer(r->version, current);
        LOG_INFO("Update", "latest release {} ({} MB), this is {}: {}", r->version, r->size >> 20, current,
                 newer ? "update available" : "up to date");
        std::lock_guard lk(mutex);
        release = *r;
        state = newer ? State::Available : State::UpToDate;
    });
}

void Updater::Start(const fs::path& keep) {
    if (state != State::Available)
        return;
    Join();
    state = State::Downloading;
    progress = 0;
    thread = std::thread([this, keep] {
        auto fail = [this](const std::string& why) {
            LOG_ERROR("Update", "update: {}", why);
            std::lock_guard lk(mutex);
            error = why;
            state = State::Failed;
        };
        Release r;
        {
            std::lock_guard lk(mutex);
            r = release;
        }
        std::error_code ec;
        const fs::path root = PackagesDir();
        // a version name is a folder name here: digits, letters, dots and dashes only
        std::string name;
        for (char c : r.version)
            if (std::isalnum(u8(c)) || c == '.' || c == '-')
                name += c;
        if (name.empty())
            return fail("the release has no usable version name");
        const fs::path dir = root / name, zip = root / (name + ".zip"), part = root / (name + ".zip.part");
        fs::create_directories(root, ec);
        fs::remove(part, ec);
        LOG_INFO("Update", "downloading {}", r.url);
        const int dl = RunHidden(SystemExe(L"curl.exe"),
                                 L"-L -f -s --retry 2 -A MUA3Native-launcher -o \"" + part.wstring() + L"\" " +
                                     fs::u8path(r.url).wstring(),
                                 {}, [&] {
                                     std::error_code e2;
                                     const u64 have = fs::exists(part, e2) ? fs::file_size(part, e2) : 0;
                                     if (r.size)
                                         progress = std::min(0.99f, float(double(have) / double(r.size)));
                                 });
        const u64 got = fs::exists(part, ec) ? fs::file_size(part, ec) : 0;
        if (dl != 0 || got < 1000000 || (r.size && got != r.size)) {
            fs::remove(part, ec);
            return fail(fmt::format("the download failed (curl {}, {} of {} bytes). Check the internet connection, or "
                                    "download the new version from GitHub.",
                                    dl, got, r.size));
        }
        fs::remove(zip, ec);
        fs::rename(part, zip, ec);
        if (ec)
            return fail("could not keep the download: " + ec.message());
        state = State::Unpacking;
        progress = 1;
        fs::remove_all(dir, ec);
        fs::create_directories(dir, ec);
        const int un = RunHidden(SystemExe(L"tar.exe"), L"-xf \"" + zip.wstring() + L"\"", dir, [] {});
        fs::remove(zip, ec);
        fs::path exe;
        for (auto it = fs::recursive_directory_iterator(dir, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (it.depth() > 1)
                it.disable_recursion_pending();
            if (it->is_regular_file(ec) && _wcsicmp(it->path().filename().c_str(), L"MUA3Setup.exe") == 0) {
                exe = it->path();
                break;
            }
        }
        if (un != 0 || exe.empty() || !fs::exists(exe.parent_path() / "sdk" / "tools" / "recomp.exe")) {
            fs::remove_all(dir, ec);
            return fail(fmt::format("the package could not be unpacked (tar {})", un));
        }
        // older packages go (not the one the install was built from, nor the new one)
        for (const auto& e : fs::directory_iterator(root, ec)) {
            if (!e.is_directory() || fs::equivalent(e.path(), dir, ec))
                continue;
            const fs::path rel = keep.empty() ? fs::path() : keep.lexically_relative(e.path());
            if (!rel.empty() && *rel.begin() != "..")
                continue;
            fs::remove_all(e.path(), ec);
        }
        LOG_INFO("Update", "{} unpacked: {}", r.version, exe.string());
        std::lock_guard lk(mutex);
        setup_exe = exe;
        state = State::Ready;
    });
}

bool Updater::Run(const fs::path& install_dir) {
    if (state != State::Ready)
        return false;
    fs::path exe;
    {
        std::lock_guard lk(mutex);
        exe = setup_exe;
    }
    SHELLEXECUTEINFOW sei{sizeof(sei)};
    sei.fMask = SEE_MASK_NOASYNC;
    sei.lpVerb = L"open";
    sei.lpFile = exe.c_str();
    const std::wstring args = L"--update \"" + install_dir.wstring() + L"\"";
    sei.lpParameters = args.c_str();
    const std::wstring wd = exe.parent_path().wstring();
    sei.lpDirectory = wd.c_str();
    sei.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&sei)) {
        std::lock_guard lk(mutex);
        error = fmt::format("could not start the new setup (error {})", GetLastError());
        state = State::Failed;
        return false;
    }
    LOG_INFO("Update", "started {} --update {}", exe.string(), install_dir.string());
    return true;
}

} // namespace updater
