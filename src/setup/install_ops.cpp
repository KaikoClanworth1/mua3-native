#include "setup/install_ops.h"

#include <algorithm>
#include <fstream>
#include <regex>

#include <windows.h>
#include <knownfolders.h>
#include <objbase.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wincodec.h>

#include "common/crash.h"
#include "crypto/crypto.h"
#include "loader/formats.h"

#pragma comment(lib, "windowscodecs.lib")

namespace setup {

namespace {

constexpr const wchar_t* kUninstallRoot = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\";
constexpr const char* kSetupIni = "setup.ini"; // in the install folder: what setup made

std::wstring UninstallKey() {
    return std::wstring(kUninstallRoot) + (Sandbox() ? L"MUA3Native-SetupTest" : L"MUA3Native");
}

std::string Lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), ::tolower);
    return s;
}

fs::path KnownFolder(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    fs::path out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p)))
        out = p;
    CoTaskMemFree(p);
    return out;
}

std::string U8(const fs::path& p) {
    const auto s = p.u8string();
    return std::string(s.begin(), s.end());
}

fs::path Env(const wchar_t* name) {
    const wchar_t* v = _wgetenv(name);
    return v ? fs::path(v) : fs::path();
}

// The files of an .nsp (PFS0) or .xci (secure partition).
struct Container {
    loader::StoragePtr storage;
    std::vector<loader::PfsEntry> entries;
};
std::optional<Container> Open(const fs::path& p) {
    auto file = loader::FileStorage::Open(p);
    if (!file)
        return std::nullopt;
    const std::string ext = Lower(p.extension().string());
    Container c{file, {}};
    if (ext == ".nsp") {
        auto e = loader::ParsePfs0(*file);
        if (!e)
            return std::nullopt;
        c.entries = std::move(*e);
    } else if (ext == ".xci") {
        loader::StoragePtr secure;
        auto e = loader::ParseXciSecure(file, &secure);
        if (!e)
            return std::nullopt;
        c.storage = secure;
        c.entries = std::move(*e);
    } else {
        return std::nullopt;
    }
    return c;
}

bool EndsWith(const std::string& s, const char* suffix) {
    const size_t n = std::strlen(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

bool LoadKeys(const fs::path& prod, crypto::KeySet& keys) {
    if (prod.empty() || !keys.LoadProdKeys(prod))
        return false;
    keys.LoadTitleKeys(prod.parent_path() / "title.keys");
    return true;
}

void AddTickets(const Container& c, crypto::KeySet& keys) {
    for (auto& e : c.entries)
        if (EndsWith(e.name, ".tik"))
            if (auto t = loader::ParseTicket(loader::SubStorage(c.storage, e.offset, e.size)))
                keys.AddTitleKey(t->rights_id, t->title_key);
}

// Title id and version from the content meta NCA (header key only: meta NCAs have no title key).
bool ReadMeta(const Container& c, const crypto::KeySet& keys, std::string& tid, u32& version) {
    for (auto& e : c.entries) {
        if (!EndsWith(e.name, ".cnmt.nca"))
            continue;
        auto nca = loader::Nca::Open(std::make_shared<loader::SubStorage>(c.storage, e.offset, e.size), keys);
        if (!nca)
            continue;
        const int si = nca->FindSection(1);
        auto pfs = si >= 0 ? nca->Pfs0Storage(si) : nullptr;
        auto files = pfs ? loader::ParsePfs0(*pfs) : std::nullopt;
        if (!files)
            continue;
        for (auto& f : *files) {
            if (!EndsWith(f.name, ".cnmt") || f.size < 0x20)
                continue;
            const u64 id = pfs->ReadObj<u64>(f.offset);
            version = pfs->ReadObj<u32>(f.offset + 8);
            tid = fmt::format("{:016X}", id);
            return true;
        }
    }
    return false;
}

} // namespace

const char* Sandbox() {
    const char* s = std::getenv("MUA3_SETUP_SANDBOX");
    return s && *s ? s : nullptr;
}

std::string Gb(u64 bytes) {
    const double gb = double(bytes) / double(1ull << 30);
    return gb >= 10 ? fmt::format("{:.0f} GB", gb) : fmt::format("{:.1f} GB", gb);
}

std::string GameFile::Describe() const {
    std::string what;
    switch (kind) {
    case FileKind::Base: what = "Base game"; break;
    case FileKind::Update: what = "Update"; break;
    case FileKind::Dlc: what = "DLC"; break;
    case FileKind::OtherGame: what = "Another game"; break;
    default: what = "Unknown file"; break;
    }
    if (version)
        what += kind == FileKind::Update && *version == kSupportedUpdate ? fmt::format(" {} (v{})", kSupportedUpdateName, *version)
                                                                         : fmt::format(" · v{}", *version);
    what += " · " + (size >= (1ull << 30) ? Gb(size)
                     : size >= (1u << 20) ? fmt::format("{:.0f} MB", double(size) / (1 << 20))
                                          : fmt::format("{:.0f} KB", double(size) / 1024));
    return what;
}

std::string GameFile::Badge() const {
    switch (verdict) {
    case Verdict::Supported: return "Supported version";
    case Verdict::WrongVersion: return "Wrong version";
    case Verdict::WrongGame: return "Not this game";
    default: return "Not checked";
    }
}

GameFile Probe(const fs::path& path, const fs::path& prod) {
    GameFile g;
    g.path = path;
    std::error_code ec;
    g.size = fs::file_size(path, ec);
    const std::string name = path.filename().string();
    std::string tid;
    {
        static const std::regex re(R"(\[(0100[0-9A-Fa-f]{12})\])");
        std::smatch m;
        if (std::regex_search(name, m, re))
            tid = m[1];
        static const std::regex rv(R"(\[v(\d+)\])");
        if (std::regex_search(name, m, rv))
            g.version = u32(std::stoul(m[1]));
    }
    auto c = Open(path);
    if (c) {
        for (auto& f : c->entries)
            if (EndsWith(f.name, ".tik") && f.name.size() >= 16)
                tid = f.name.substr(0, 16);
        crypto::KeySet keys;
        u32 ver = 0;
        std::string meta_tid;
        if (LoadKeys(prod, keys) && ReadMeta(*c, keys, meta_tid, ver)) {
            tid = meta_tid;
            g.version = ver;
        }
    }
    std::transform(tid.begin(), tid.end(), tid.begin(), ::toupper);
    g.title_id = tid;
    const bool ours = tid.size() == 16 && tid.compare(0, 12, "010060700AC5") == 0;
    if (tid.size() == 16 && !ours) {
        g.kind = FileKind::OtherGame;
    } else if (ours) {
        const std::string low = tid.substr(12);
        g.kind = low == "0000" ? FileKind::Base : low == "0800" ? FileKind::Update : FileKind::Dlc;
    } else if (Lower(path.extension().string()) == ".xci" && c) {
        g.kind = FileKind::Base; // cartridge dump: the base game
    }
    switch (g.kind) {
    case FileKind::OtherGame: g.verdict = Verdict::WrongGame; break;
    case FileKind::Update:
        g.verdict = !g.version ? Verdict::Unknown : *g.version == kSupportedUpdate ? Verdict::Supported : Verdict::WrongVersion;
        break;
    case FileKind::Base:
    case FileKind::Dlc: g.verdict = c ? Verdict::Supported : Verdict::Unknown; break;
    default: g.verdict = Verdict::Unknown; break;
    }
    g.selected = g.verdict == Verdict::Supported;
    return g;
}

std::vector<fs::path> CandidateFolders() {
    std::vector<fs::path> out;
    const fs::path here = SetupDir();
    for (const char* sub : {"Isos", "isos", "Games", "games"})
        out.push_back(here / sub);
    out.push_back(here);
    out.push_back(here.parent_path() / "Isos");
    out.push_back(fs::current_path() / "Isos");
    out.push_back(KnownFolder(FOLDERID_Downloads));
    out.push_back(KnownFolder(FOLDERID_Desktop));
    out.push_back(KnownFolder(FOLDERID_Documents));
    // Folders named like a game library at the top of each fixed drive.
    const DWORD drives = GetLogicalDrives();
    for (int d = 2; d < 26; d++) { // C: onwards
        if (!(drives & (1u << d)))
            continue;
        const std::wstring root = std::wstring(1, wchar_t(L'A' + d)) + L":\\";
        if (GetDriveTypeW(root.c_str()) != DRIVE_FIXED)
            continue;
        for (const char* name : {"Games", "Switch", "Isos", "ROMs", "Roms", "NSP", "Switch Games"})
            out.push_back(fs::path(root) / name);
    }
    std::vector<fs::path> unique;
    std::error_code ec;
    for (auto& p : out) {
        if (p.empty() || !fs::is_directory(p, ec))
            continue;
        bool dup = false;
        for (auto& u : unique)
            dup |= fs::equivalent(u, p, ec);
        if (!dup)
            unique.push_back(p);
    }
    return unique;
}

std::vector<fs::path> FindGameFiles(const std::vector<fs::path>& folders, std::atomic<bool>& cancel) {
    std::vector<fs::path> out;
    std::error_code ec;
    auto consider = [&](const fs::path& p) {
        const std::string ext = Lower(p.extension().string());
        if (ext != ".nsp" && ext != ".xci")
            return;
        // Marvel Ultimate Alliance 3 only: by title id in the name, or by the name itself, or a big unnamed dump.
        const std::string n = Lower(p.filename().string());
        const bool named = n.find("010060700ac5") != std::string::npos || n.find("marvel") != std::string::npos ||
                           n.find("ultimate alliance") != std::string::npos || n.find("mua3") != std::string::npos;
        if (!named)
            return;
        for (auto& o : out)
            if (fs::equivalent(o, p, ec))
                return;
        out.push_back(p);
    };
    for (auto& f : folders) {
        for (auto it = fs::directory_iterator(f, fs::directory_options::skip_permission_denied, ec);
             !ec && it != fs::directory_iterator() && !cancel; it.increment(ec)) {
            if (it->is_regular_file(ec)) {
                consider(it->path());
            } else if (it->is_directory(ec)) {
                std::error_code ec2;
                int n = 0;
                for (auto jt = fs::directory_iterator(it->path(), fs::directory_options::skip_permission_denied, ec2);
                     !ec2 && jt != fs::directory_iterator() && n < 2000 && !cancel; jt.increment(ec2), n++)
                    if (jt->is_regular_file(ec2))
                        consider(jt->path());
            }
        }
        ec.clear();
    }
    return out;
}

std::vector<fs::path> FindKeys() {
    std::vector<fs::path> dirs = {SetupDir() / "keys", SetupDir().parent_path() / "keys", fs::current_path() / "keys"};
    const fs::path appdata = Env(L"APPDATA");
    for (const char* emu : {"yuzu", "suyu", "sudachi", "eden", "citron", "torzu"})
        dirs.push_back(appdata / emu / "keys");
    dirs.push_back(appdata / "Ryujinx" / "system");
    dirs.push_back(KnownFolder(FOLDERID_Profile) / ".switch");
    std::vector<fs::path> out;
    std::error_code ec;
    for (auto& d : dirs)
        if (fs::exists(d / "prod.keys", ec))
            out.push_back(d / "prod.keys");
    return out;
}

std::vector<u8> ReadGameIcon(const std::vector<fs::path>& files, const fs::path& prod) {
    crypto::KeySet keys;
    if (!LoadKeys(prod, keys))
        return {};
    std::vector<Container> cs;
    for (auto& f : files)
        if (auto c = Open(f)) {
            AddTickets(*c, keys);
            cs.push_back(std::move(*c));
        }
    std::vector<u8> best;
    for (auto& c : cs)
        for (auto& e : c.entries) {
            if (!EndsWith(e.name, ".nca") || EndsWith(e.name, ".cnmt.nca") || e.size > (64ull << 20))
                continue; // control NCAs are small
            auto nca = loader::Nca::Open(std::make_shared<loader::SubStorage>(c.storage, e.offset, e.size), keys);
            if (!nca || nca->ContentType() != loader::NcaContentType::Control)
                continue;
            const int si = nca->FindSection(0);
            auto romfs = si >= 0 ? nca->RomfsStorage(si) : nullptr;
            auto list = romfs ? loader::ParseRomfs(*romfs) : std::nullopt;
            if (!list)
                continue;
            for (auto& f : *list)
                if (f.path == "icon_AmericanEnglish.dat" && f.size < (4u << 20))
                    return romfs->ReadBytes(f.offset, size_t(f.size));
                else if (best.empty() && f.path.rfind("icon_", 0) == 0 && f.size < (4u << 20))
                    best = romfs->ReadBytes(f.offset, size_t(f.size));
        }
    return best;
}

Space DriveSpace(const fs::path& p) {
    Space s;
    std::error_code ec;
    fs::path probe = fs::absolute(p, ec);
    s.drive = probe.root_name().string();
    while (!probe.empty() && !fs::exists(probe, ec) && probe != probe.parent_path())
        probe = probe.parent_path();
    const auto sp = fs::space(probe, ec);
    if (!ec)
        s.free = sp.available, s.total = sp.capacity;
    return s;
}

u64 InstallBytes(const std::vector<GameFile>& files) {
    // The game folder holds the base game's data with the update applied (about the base dump's size; the update
    // replaces files rather than adding them), the DLC, the built game and its shaders.
    u64 base = 0, update = 0, dlc = 0;
    for (auto& f : files) {
        if (!f.selected)
            continue;
        if (f.kind == FileKind::Base)
            base += f.size;
        else if (f.kind == FileKind::Update)
            update += f.size;
        else
            dlc += f.size;
    }
    return base + update / 10 * 7 + dlc + (1ull << 30); // (measured: 14.2 + 4.9 GB of dumps = 17.8 GB of data)
}

fs::path SetupDir() {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    return fs::path(exe).parent_path();
}

fs::path LocalDataDir() {
    if (const char* s = Sandbox())
        return fs::u8path(s) / "LocalAppData";
    return KnownFolder(FOLDERID_LocalAppData) / "MUA3Native";
}

fs::path DefaultInstallDir() {
    // The drive with the most room among the fixed ones, in a Games folder.
    fs::path best = "C:\\";
    u64 best_free = 0;
    const DWORD drives = GetLogicalDrives();
    for (int d = 2; d < 26; d++) {
        if (!(drives & (1u << d)))
            continue;
        const std::wstring root = std::wstring(1, wchar_t(L'A' + d)) + L":\\";
        if (GetDriveTypeW(root.c_str()) != DRIVE_FIXED)
            continue;
        ULARGE_INTEGER avail{};
        if (GetDiskFreeSpaceExW(root.c_str(), &avail, nullptr, nullptr) && avail.QuadPart > best_free)
            best = root, best_free = avail.QuadPart;
    }
    return best / "Games" / "Marvel Ultimate Alliance 3";
}

fs::path DefaultBuildDir() { return LocalDataDir() / "build"; }

bool MakeIco(const fs::path& jpeg, const fs::path& ico) {
    IWICImagingFactory* wic = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))))
        return false;
    IWICBitmapDecoder* dec = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    bool ok = SUCCEEDED(wic->CreateDecoderFromFilename(jpeg.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &dec)) &&
              SUCCEEDED(dec->GetFrame(0, &frame));
    const UINT sizes[] = {256, 128, 64, 48, 32, 24, 16};
    std::vector<std::vector<u8>> pngs;
    for (UINT s : sizes) {
        if (!ok)
            break;
        IWICBitmapScaler* scaler = nullptr;
        IWICFormatConverter* conv = nullptr;
        IStream* stream = nullptr;
        IWICBitmapEncoder* enc = nullptr;
        IWICBitmapFrameEncode* out = nullptr;
        ok = SUCCEEDED(wic->CreateBitmapScaler(&scaler)) &&
             SUCCEEDED(scaler->Initialize(frame, s, s, WICBitmapInterpolationModeHighQualityCubic)) &&
             SUCCEEDED(wic->CreateFormatConverter(&conv)) &&
             SUCCEEDED(conv->Initialize(scaler, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                        WICBitmapPaletteTypeCustom)) &&
             SUCCEEDED(CreateStreamOnHGlobal(nullptr, TRUE, &stream)) &&
             SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) &&
             SUCCEEDED(enc->Initialize(stream, WICBitmapEncoderNoCache)) &&
             SUCCEEDED(enc->CreateNewFrame(&out, nullptr)) && SUCCEEDED(out->Initialize(nullptr)) &&
             SUCCEEDED(out->SetSize(s, s));
        WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
        ok = ok && SUCCEEDED(out->SetPixelFormat(&fmt)) && SUCCEEDED(out->WriteSource(conv, nullptr)) &&
             SUCCEEDED(out->Commit()) && SUCCEEDED(enc->Commit());
        if (ok) {
            HGLOBAL h = nullptr;
            GetHGlobalFromStream(stream, &h);
            STATSTG st{};
            stream->Stat(&st, STATFLAG_NONAME);
            const u8* p = static_cast<const u8*>(GlobalLock(h));
            pngs.emplace_back(p, p + st.cbSize.QuadPart);
            GlobalUnlock(h);
        }
        if (out) out->Release();
        if (enc) enc->Release();
        if (stream) stream->Release();
        if (conv) conv->Release();
        if (scaler) scaler->Release();
    }
    if (frame) frame->Release();
    if (dec) dec->Release();
    wic->Release();
    if (!ok || pngs.size() != std::size(sizes))
        return false;
    std::vector<u8> f;
    auto put16 = [&](u16 v) { f.push_back(u8(v)), f.push_back(u8(v >> 8)); };
    auto put32 = [&](u32 v) { put16(u16(v)), put16(u16(v >> 16)); };
    put16(0), put16(1), put16(u16(pngs.size()));
    u32 offset = 6 + 16 * u32(pngs.size());
    for (size_t i = 0; i < pngs.size(); i++) {
        f.push_back(u8(sizes[i] % 256)), f.push_back(u8(sizes[i] % 256)); // 256 is stored as 0
        f.push_back(0), f.push_back(0);
        put16(1), put16(32);
        put32(u32(pngs[i].size())), put32(offset);
        offset += u32(pngs[i].size());
    }
    for (auto& p : pngs)
        f.insert(f.end(), p.begin(), p.end());
    std::error_code ec;
    fs::create_directories(ico.parent_path(), ec);
    return common::WriteFile(ico, f);
}

bool CreateShortcut(const fs::path& lnk, const fs::path& target, const fs::path& workdir, const fs::path& icon,
                    const std::wstring& description) {
    IShellLinkW* link = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link))))
        return false;
    link->SetPath(target.c_str());
    link->SetWorkingDirectory(workdir.c_str());
    link->SetDescription(description.c_str());
    if (!icon.empty())
        link->SetIconLocation(icon.c_str(), 0);
    IPersistFile* file = nullptr;
    bool ok = false;
    std::error_code ec;
    fs::create_directories(lnk.parent_path(), ec);
    if (SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&file)))) {
        ok = SUCCEEDED(file->Save(lnk.c_str(), TRUE));
        file->Release();
    }
    link->Release();
    return ok;
}

fs::path DesktopShortcut() {
    const fs::path dir = Sandbox() ? fs::u8path(Sandbox()) / "Desktop" : KnownFolder(FOLDERID_Desktop);
    return dir / "Marvel Ultimate Alliance 3.lnk";
}

fs::path StartMenuShortcut() {
    const fs::path dir = Sandbox() ? fs::u8path(Sandbox()) / "StartMenu" : KnownFolder(FOLDERID_Programs);
    return dir / "Marvel Ultimate Alliance 3.lnk";
}

bool SaveInstalled(const Installed& in) {
    std::ofstream f(in.dir / kSetupIni);
    f << "# Written by MUA3Setup: what it installed (for repair, update and uninstall).\n";
    f << "build_dir=" << U8(in.build_dir) << "\nsource_dir=" << U8(in.source_dir)
      << "\nkeys=" << U8(in.keys) << "\nlanguage=" << in.language << "\ndesktop=" << in.desktop
      << "\nstart_menu=" << in.start_menu << "\n";
    for (auto& p : in.files)
        f << "file=" << U8(p) << "\n";
    return bool(f);
}

std::optional<Installed> LoadInstalled(const fs::path& dir) {
    std::ifstream f(dir / kSetupIni);
    if (!f)
        return std::nullopt;
    Installed in;
    in.dir = dir;
    for (std::string line; std::getline(f, line);) {
        const auto eq = line.find('=');
        if (line.empty() || line[0] == '#' || eq == std::string::npos)
            continue;
        const std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        if (k == "build_dir") in.build_dir = fs::u8path(v);
        else if (k == "source_dir") in.source_dir = fs::u8path(v);
        else if (k == "keys") in.keys = fs::u8path(v);
        else if (k == "language") in.language = std::atoi(v.c_str());
        else if (k == "desktop") in.desktop = v == "1";
        else if (k == "start_menu") in.start_menu = v == "1";
        else if (k == "file") in.files.push_back(fs::u8path(v));
    }
    return in;
}

std::optional<Installed> FindInstalled() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, UninstallKey().c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS)
        return std::nullopt;
    wchar_t buf[MAX_PATH * 2]{};
    DWORD size = sizeof(buf) - sizeof(wchar_t);
    const bool ok = RegQueryValueExW(key, L"InstallLocation", nullptr, nullptr, reinterpret_cast<BYTE*>(buf), &size) == ERROR_SUCCESS;
    RegCloseKey(key);
    if (!ok)
        return std::nullopt;
    const fs::path dir = buf;
    if (auto in = LoadInstalled(dir))
        return in;
    Installed in; // the entry without setup.ini: still uninstallable
    in.dir = dir;
    return in;
}

bool RegisterUninstall(const Installed& in, u64 bytes) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, UninstallKey().c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr) !=
        ERROR_SUCCESS)
        return false;
    auto str = [&](const wchar_t* name, const std::wstring& v) {
        RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(v.c_str()), DWORD((v.size() + 1) * sizeof(wchar_t)));
    };
    auto dword = [&](const wchar_t* name, DWORD v) { RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&v), 4); };
    const std::wstring setup = (in.dir / "MUA3Setup.exe").wstring();
    str(L"DisplayName", L"Marvel Ultimate Alliance 3: The Black Order (MUA3 Native)");
    str(L"DisplayIcon", (in.dir / "mua3.ico").wstring());
    str(L"DisplayVersion", fs::u8path(common::Version()).wstring());
    str(L"Publisher", L"MUA3 Native (built on this PC from your own game files)");
    str(L"InstallLocation", in.dir.wstring());
    str(L"UninstallString", L"\"" + setup + L"\" --uninstall");
    str(L"ModifyPath", L"\"" + setup + L"\"");
    str(L"URLInfoAbout", L"https://github.com/KaikoClanworth1/mua3-native");
    dword(L"EstimatedSize", DWORD(bytes >> 10));
    dword(L"NoRepair", 1);
    RegCloseKey(key);
    return true;
}

void UnregisterUninstall() { RegDeleteKeyW(HKEY_CURRENT_USER, UninstallKey().c_str()); }

u64 FolderBytes(const fs::path& dir) {
    u64 total = 0;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
        if (it->is_regular_file(ec))
            total += it->file_size(ec);
    return total;
}

std::vector<std::string> Uninstall(const Installed& in, bool keep_saves) {
    std::vector<std::string> left;
    std::error_code ec;
    auto remove_all = [&](const fs::path& p) {
        if (!fs::exists(p, ec))
            return;
        fs::remove_all(p, ec);
        if (ec)
            left.push_back(p.string() + ": " + ec.message());
        ec.clear();
    };
    // Shortcuts (only ours: they point at this install's mua3.exe) and the Apps entry.
    for (const fs::path& lnk : {DesktopShortcut(), StartMenuShortcut()})
        remove_all(lnk);
    UnregisterUninstall();
    // The build files (always setup's own folder: it is made for the build).
    if (!in.build_dir.empty())
        remove_all(in.build_dir);
    remove_all(LocalDataDir() / "icon.jpg"); // the game's icon, cached for setup's window
    // What setup and the game wrote in the install folder. Anything else the player put there stays.
    if (!in.dir.empty() && (fs::exists(in.dir / "mua3.exe", ec) || fs::exists(in.dir / kSetupIni, ec))) {
        for (const char* p : {"mua3.exe", "mua3.ini", "mua3.ico", "mua3.log", "mua3.pdb", "setup.ini", "game",
                              "shader_cache", "shader_dump", "logs", "crash", "romfs_index.bin", "launcher.log"})
            remove_all(in.dir / p);
        for (auto it = fs::directory_iterator(in.dir, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
            const std::string n = Lower(it->path().filename().string());
            if (n.rfind("mua3_crash", 0) == 0 || n.rfind("crash_", 0) == 0 || n.find(".dmp") != std::string::npos)
                fs::remove(it->path(), ec);
        }
        ec.clear();
        if (!keep_saves)
            for (const char* p : {"save", "screenshots"})
                remove_all(in.dir / p);
        // MUA3Setup.exe itself is removed by the caller after this process ends (it may be running from here).
    }
    return left;
}

} // namespace setup
