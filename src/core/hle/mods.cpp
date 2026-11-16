// Mods: finding mod folders, mods\order.txt and mods\mods.log. The merge itself is in services/fs.cpp.
#include "core/hle/mods.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <set>
#include <unordered_map>

#include "core/system.h"

namespace hle::mods {
namespace {

constexpr const char* kOrderFile = "order.txt";
constexpr const char* kLogFile = "mods.log";
// MUA3's RomFS top-level folders: a mod folder holding any of them is a bare RomFS
constexpr const char* kTopDirs[] = {"ACTION", "AI",     "DATA",  "EVENT",        "Effect", "FONT", "MOVIE",
                                    "SCENARIO", "SHADER", "SOUND", "STAGE", "STAGE_GADGET", "UI"};

std::string Lower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z')
            c = char(c - 'A' + 'a');
    return s;
}
bool IEq(const std::string& a, const std::string& b) { return Lower(a) == Lower(b); }

std::string U8(const fs::path& p) {
    const auto s = p.u8string();
    return std::string(s.begin(), s.end());
}

std::vector<fs::path> SubDirs(const fs::path& dir) {
    std::vector<fs::path> out;
    std::error_code ec;
    for (auto& e : fs::directory_iterator(dir, ec))
        if (e.is_directory(ec))
            out.push_back(e.path());
    std::sort(out.begin(), out.end());
    return out;
}

// the child folder of `dir` named `name`, any case; empty when there is none
fs::path Child(const fs::path& dir, const std::string& name) {
    if (dir.empty())
        return {};
    for (const fs::path& d : SubDirs(dir))
        if (IEq(U8(d.filename()), name))
            return d;
    return {};
}

void NoteUnsupported(const fs::path& base, Mod& m) {
    if (!Child(base, "exefs").empty())
        m.unsupported.push_back("exefs (code patches: the game's code is recompiled ahead of time, they cannot apply)");
    if (!Child(base, "cheats").empty())
        m.unsupported.push_back("cheats (not supported)");
    std::error_code ec;
    for (auto& e : fs::directory_iterator(base, ec)) {
        const std::string ext = Lower(U8(e.path().extension()));
        if (ext == ".ips" || ext == ".pchtxt")
            m.unsupported.push_back(U8(e.path().filename()) + " (code patch: cannot apply)");
    }
}

// Finds the RomFS folder of mod folder `dir` (see mods.h for the layouts). `depth`: a mod zip often extracts into
// one more folder level, which is looked through once.
bool FindRomfs(const fs::path& dir, Mod& m, const std::string& prefix = {}, int depth = 0) {
    struct Try {
        fs::path base;
        const char* layout;
    };
    const fs::path tid = Child(dir, kTitleId);
    const Try tries[] = {
        {dir, "romfs"},
        {tid, "010060700AC50000\\romfs"},
        {Child(Child(Child(dir, "atmosphere"), "contents"), kTitleId), "atmosphere\\contents\\010060700AC50000\\romfs"},
        {Child(Child(dir, "contents"), kTitleId), "contents\\010060700AC50000\\romfs"},
    };
    for (const Try& t : tries) {
        if (t.base.empty())
            continue;
        if (fs::path r = Child(t.base, "romfs"); !r.empty()) {
            m.romfs = r, m.layout = prefix + t.layout;
            NoteUnsupported(t.base, m);
            return true;
        }
    }
    for (const char* top : kTopDirs)
        if (!Child(dir, top).empty()) {
            m.romfs = dir, m.layout = prefix + "RomFS folders";
            NoteUnsupported(dir, m);
            return true;
        }
    for (const Try& t : tries) // no RomFS part, but maybe code patches or cheats to tell the player about
        if (!t.base.empty())
            NoteUnsupported(t.base, m);
    if (depth == 0) {
        const auto subs = SubDirs(dir);
        if (subs.size() == 1 && m.unsupported.empty())
            return FindRomfs(subs[0], m, prefix + U8(subs[0].filename()) + "\\", 1);
    }
    return false;
}

struct OrderLine {
    std::string name;
    bool enabled = true;
};

std::vector<OrderLine> ReadOrder(bool* exists) {
    std::vector<OrderLine> out;
    std::ifstream f(ModsDir() / kOrderFile);
    *exists = bool(f);
    std::string line;
    bool first = true;
    while (std::getline(f, line)) {
        if (first && line.size() >= 3 && line.compare(0, 3, "\xEF\xBB\xBF") == 0) // UTF-8 BOM (Notepad)
            line.erase(0, 3);
        first = false;
        auto trim = [](std::string& s) {
            while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r'))
                s.pop_back();
            size_t i = 0;
            while (i < s.size() && (s[i] == ' ' || s[i] == '\t'))
                i++;
            s.erase(0, i);
        };
        trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';')
            continue;
        OrderLine o;
        if (line[0] == '-' || line[0] == '+') {
            o.enabled = line[0] == '+';
            line.erase(0, 1);
            trim(line);
        }
        o.name = line;
        if (!o.name.empty())
            out.push_back(o);
    }
    return out;
}

std::mutex g_mutex;
std::vector<Mod> g_loaded;
bool g_launch_enabled = false, g_launch_set = false;

} // namespace

fs::path ModsDir() { return "mods"; }

bool Enabled() {
    if (const char* e = std::getenv("MUA3_MODS"); e && e[0] == '0')
        return false;
    return core::Sys().Cfg().mods_enabled;
}

std::vector<Mod> Scan(bool update_order) {
    const fs::path dir = ModsDir();
    std::error_code ec;
    if (update_order)
        fs::create_directories(dir, ec);
    // every mod folder
    std::vector<Mod> found;
    for (const fs::path& d : SubDirs(dir)) {
        const std::string name = U8(d.filename());
        if (name.empty() || name[0] == '.')
            continue;
        if (IEq(name, kTitleId)) { // an emulator's load\<title id> folder: one mod per subfolder
            for (const fs::path& s : SubDirs(d)) {
                Mod m;
                m.name = U8(s.filename());
                m.folder = s;
                FindRomfs(s, m, U8(d.filename()) + "\\" + m.name + "\\");
                found.push_back(std::move(m));
            }
            continue;
        }
        Mod m;
        m.name = name;
        m.folder = d;
        FindRomfs(d, m);
        found.push_back(std::move(m));
    }
    // in order.txt's order
    bool exists = false;
    const auto order = ReadOrder(&exists);
    std::vector<Mod> out;
    std::set<std::string> taken;
    bool changed = !exists;
    for (const OrderLine& o : order) {
        auto it = std::find_if(found.begin(), found.end(), [&](const Mod& m) { return IEq(m.name, o.name); });
        if (it == found.end() || taken.count(Lower(o.name))) {
            changed = true; // gone (or listed twice)
            continue;
        }
        taken.insert(Lower(o.name));
        it->enabled = o.enabled;
        out.push_back(*it);
    }
    std::vector<Mod> added;
    for (const Mod& m : found)
        if (!taken.count(Lower(m.name)) && taken.insert(Lower(m.name)).second)
            added.push_back(m);
    std::sort(added.begin(), added.end(), [](const Mod& a, const Mod& b) { return Lower(a.name) < Lower(b.name); });
    changed |= !added.empty();
    out.insert(out.end(), added.begin(), added.end());
    if (update_order && changed)
        SaveOrder(out);
    return out;
}

bool SaveOrder(const std::vector<Mod>& mods) {
    std::error_code ec;
    fs::create_directories(ModsDir(), ec);
    const fs::path path = ModsDir() / kOrderFile, tmp = ModsDir() / "order.txt.tmp";
    {
        std::ofstream f(tmp, std::ios::binary);
        f << "# MUA3 mods, in load order: mods lower in the list win when two change the same file.\r\n"
             "# A '-' in front of a name turns that mod off. New mod folders are added at the bottom.\r\n"
             "# Changes apply the next time the game starts. See docs/mods.md.\r\n";
        for (const Mod& m : mods)
            f << (m.enabled ? "" : "-") << m.name << "\r\n";
        if (!f)
            return false;
    }
    fs::rename(tmp, path, ec);
    return !ec;
}

fs::path ResolveLoose(const fs::path& game_dir, std::string_view rel, bool mods_enabled) {
    const fs::path r = fs::u8path(std::string(rel));
    if (mods_enabled) {
        const auto mods = Scan(false);
        std::error_code ec;
        for (auto it = mods.rbegin(); it != mods.rend(); ++it) // later mods win
            if (it->enabled && !it->romfs.empty() && fs::is_regular_file(it->romfs / r, ec))
                return it->romfs / r;
    }
    return game_dir / "romfs" / r;
}

std::vector<Mod> Loaded() {
    std::lock_guard lk(g_mutex);
    return g_loaded;
}

bool ChangedSinceLaunch() {
    std::vector<std::string> now, then;
    bool launch_enabled;
    {
        std::lock_guard lk(g_mutex);
        if (!g_launch_set)
            return false;
        launch_enabled = g_launch_enabled;
        for (const Mod& m : g_loaded)
            then.push_back(Lower(m.name));
    }
    if (Enabled() != launch_enabled)
        return true;
    if (!launch_enabled)
        return false;
    for (const Mod& m : Scan(false))
        if (m.enabled && !m.romfs.empty())
            now.push_back(Lower(m.name));
    return now != then;
}

void SetLoaded(std::vector<Mod> mods) {
    std::lock_guard lk(g_mutex);
    g_loaded.clear();
    for (Mod& m : mods)
        if (m.loaded)
            g_loaded.push_back(std::move(m));
    g_launch_enabled = Enabled();
    g_launch_set = true;
}

void WriteLog(const std::vector<Mod>& mods, const std::vector<std::string>& lines) {
    std::ofstream f(ModsDir() / kLogFile, std::ios::binary);
    if (!f)
        return;
    f << "MUA3 mods loaded at the last launch (mods\\order.txt order; later mods win)\r\n\r\n";
    for (const Mod& m : mods) {
        f << (m.enabled ? "[on]  " : "[off] ") << m.name;
        if (m.romfs.empty())
            f << ": no RomFS files found (expected romfs\\, 010060700AC50000\\romfs\\ or game folders like DATA\\)";
        else if (m.enabled)
            f << ": " << m.overrides << " replaced, " << m.addons << " added (" << m.layout << ")";
        f << "\r\n";
        for (const std::string& u : m.unsupported)
            f << "      not supported: " << u << "\r\n";
    }
    f << "\r\n";
    for (const std::string& l : lines)
        f << l << "\r\n";
}

} // namespace hle::mods
