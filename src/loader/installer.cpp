#include "loader/installer.h"

#include <algorithm>
#include <fstream>
#include <regex>
#include <thread>

#include "loader/formats.h"

namespace loader {

namespace {

struct Container {
    fs::path path;
    StoragePtr storage;
    std::vector<PfsEntry> entries;
};

std::optional<Container> OpenContainer(const fs::path& p) {
    auto file = FileStorage::Open(p);
    if (!file)
        return std::nullopt;
    Container c{p, file, {}};
    auto ext = p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    if (ext == ".nsp") {
        auto e = ParsePfs0(*file);
        if (!e)
            return std::nullopt;
        c.entries = std::move(*e);
    } else if (ext == ".xci") {
        StoragePtr secure;
        auto e = ParseXciSecure(file, &secure);
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

std::string TitleIdFromName(const std::string& name) {
    std::smatch m;
    static const std::regex re(R"(\[(0100[0-9A-Fa-f]{12})\])");
    if (std::regex_search(name, m, re))
        return m[1];
    return {};
}

bool ExtractFile(const Storage& src, u64 offset, u64 size, const fs::path& dst, InstallProgress& prog) {
    fs::create_directories(dst.parent_path());
    std::ofstream out(dst, std::ios::binary);
    if (!out)
        return false;
    constexpr size_t kChunk = 4 << 20;
    std::vector<u8> buf(size_t(std::min<u64>(size, kChunk)));
    u64 pos = 0;
    while (pos < size) {
        if (prog.cancel)
            return false;
        const size_t n = size_t(std::min<u64>(kChunk, size - pos));
        src.Read(offset + pos, buf.data(), n);
        out.write(reinterpret_cast<const char*>(buf.data()), n);
        pos += n;
        prog.bytes_done += n;
    }
    return bool(out);
}

bool ExtractRomfs(const StoragePtr& romfs, const fs::path& dst, InstallProgress& prog, std::string& error,
                  u64* file_count = nullptr) {
    auto files = ParseRomfs(*romfs);
    if (!files) {
        error = "RomFS header is invalid (decryption problem or missing base game)";
        return false;
    }
    if (file_count)
        *file_count = files->size();
    std::sort(files->begin(), files->end(), [](auto& a, auto& b) { return a.offset < b.offset; });
    std::atomic<size_t> next{0};
    std::atomic<bool> failed{false};
    const unsigned nthreads = std::clamp(std::thread::hardware_concurrency(), 2u, 8u);
    std::vector<std::thread> pool;
    for (unsigned t = 0; t < nthreads; t++) {
        pool.emplace_back([&] {
            for (size_t i; (i = next++) < files->size() && !failed;) {
                auto& f = (*files)[i];
                if (!ExtractFile(*romfs, f.offset, f.size, dst / fs::u8path(f.path), prog))
                    failed = true;
            }
        });
    }
    for (auto& t : pool)
        t.join();
    if (failed) {
        error = prog.cancel ? "cancelled" : "failed writing RomFS files to " + dst.string();
        return false;
    }
    return true;
}

u64 RomfsBytes(const StoragePtr& romfs) {
    auto files = ParseRomfs(*romfs);
    u64 total = 0;
    if (files)
        for (auto& f : *files)
            total += f.size;
    return total;
}

} // namespace

DumpInfo ClassifyDump(const fs::path& path) {
    DumpInfo d;
    d.path = path;
    std::error_code ec;
    d.size = fs::file_size(path, ec);
    const std::string name = path.filename().string();
    auto ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    std::string tid = TitleIdFromName(name);
    if (ext == ".nsp") {
        // Ticket names inside the (unencrypted) PFS0 start with the title id.
        if (auto c = OpenContainer(path))
            for (auto& f : c->entries)
                if (EndsWith(f.name, ".tik") && f.name.size() >= 16) {
                    tid = f.name.substr(0, 16);
                } else if (EndsWith(f.name, ".cnmt.xml") && f.size < 0x10000) {
                    auto xml = c->storage->ReadBytes(f.offset, size_t(f.size));
                    const std::string x(xml.begin(), xml.end());
                    const auto p = x.find("<Id>0x");
                    if (p != std::string::npos && p + 22 <= x.size())
                        tid = x.substr(p + 6, 16);
                }
    }
    std::transform(tid.begin(), tid.end(), tid.begin(), ::toupper);
    d.title_id = tid;
    d.kind = "unknown";
    if (tid.size() == 16) {
        d.ours = tid.compare(0, 12, "010060700AC5") == 0;
        const std::string low = tid.substr(12);
        if (!d.ours)
            d.kind = "other game";
        else if (low == "0000")
            d.kind = "base game", d.base = true;
        else if (low == "0800")
            d.kind = "update", d.update = true;
        else
            d.kind = "DLC", d.dlc = true;
    }
    if (ext == ".xci" && tid.empty())
        d.kind = "cartridge (base game)", d.base = true;
    return d;
}

DumpSummary ScanDumps(const fs::path& isos_dir) {
    DumpSummary s;
    std::error_code ec;
    for (auto& e : fs::directory_iterator(isos_dir, ec)) {
        if (!e.is_regular_file())
            continue;
        auto ext = e.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (ext != ".nsp" && ext != ".xci")
            continue;
        const DumpInfo d = ClassifyDump(e.path());
        s.has_base |= d.base;
        s.has_update |= d.update;
        s.dlc_count += d.dlc;
        s.lines.push_back(fmt::format("{:<22} {:>9.2f} MB  {}", d.kind, double(d.size) / (1 << 20), e.path().filename().string()));
    }
    return s;
}

bool IsInstalled(const fs::path& out) {
    return fs::exists(out / "exefs" / "main") && fs::exists(out / "install.json");
}

bool InstallGame(const InstallOptions& opts, InstallProgress& prog, std::string& error) {
    crypto::KeySet keys;
    prog.SetStage("Loading keys");
    const fs::path prod = opts.keys_file.empty() ? opts.keys_dir / "prod.keys" : opts.keys_file;
    if (!keys.LoadProdKeys(prod)) {
        error = "Could not read " + prod.string();
        return false;
    }
    keys.LoadTitleKeys(prod.parent_path() / "title.keys");

    prog.SetStage("Scanning dumps");
    std::vector<Container> containers;
    std::error_code ec;
    if (!opts.files.empty()) {
        for (const fs::path& p : opts.files) {
            auto c = OpenContainer(p);
            if (!c) {
                error = "Not a readable .nsp/.xci: " + p.string();
                return false;
            }
            containers.push_back(std::move(*c));
        }
    } else {
        for (auto& e : fs::directory_iterator(opts.isos_dir, ec)) {
            if (!e.is_regular_file())
                continue;
            if (auto c = OpenContainer(e.path()))
                containers.push_back(std::move(*c));
        }
    }
    if (containers.empty()) {
        error = "No .nsp/.xci files found in " + opts.isos_dir.string();
        return false;
    }

    // Tickets first so rights-id NCAs can be opened.
    for (auto& c : containers)
        for (auto& e : c.entries)
            if (EndsWith(e.name, ".tik"))
                if (auto t = ParseTicket(SubStorage(c.storage, e.offset, e.size)))
                    keys.AddTitleKey(t->rights_id, t->title_key);

    struct NcaRef {
        StoragePtr storage;
        std::shared_ptr<Nca> nca;
        std::string name;
    };
    std::vector<NcaRef> ncas;
    std::vector<std::string> warnings;
    for (auto& c : containers) {
        for (auto& e : c.entries) {
            if (!EndsWith(e.name, ".nca") || EndsWith(e.name, ".cnmt.nca"))
                continue;
            auto st = std::make_shared<SubStorage>(c.storage, e.offset, e.size);
            std::string err;
            auto nca = Nca::Open(st, keys, nullptr, &err);
            if (!nca) {
                warnings.push_back(e.name + ": " + err);
                LOG_WARN("Install", "{} ({}): {}", e.name, c.path.filename().string(), err);
                continue;
            }
            LOG_INFO("Install", "{}: type {} program {:016x}{}", e.name, int(nca->ContentType()), nca->ProgramId(),
                     nca->IsPatch() ? " (patch)" : "");
            ncas.push_back({st, nca, e.name});
        }
    }

    NcaRef *base = nullptr, *update = nullptr, *control = nullptr;
    std::vector<NcaRef*> aoc;
    for (auto& r : ncas) {
        switch (r.nca->ContentType()) {
        case NcaContentType::Program:
            if (r.nca->IsPatch())
                update = &r;
            else if (!base)
                base = &r;
            break;
        case NcaContentType::Control:
            // Prefer the update's control data (newest version string).
            if (!control || (r.nca->ProgramId() & 0x800))
                control = &r;
            break;
        case NcaContentType::Data:
        case NcaContentType::PublicData:
            if ((r.nca->ProgramId() & 0xF000) != 0)
                aoc.push_back(&r);
            break;
        default: break;
        }
    }
    if (!base && !update) {
        error = "No program NCA could be opened.";
        for (auto& w : warnings)
            error += "\n  " + w;
        return false;
    }

    std::shared_ptr<Nca> program = base ? base->nca : nullptr;
    if (update) {
        program = Nca::Open(update->storage, keys, base ? base->nca : nullptr, &error);
        if (!program)
            return false;
        if (!base)
            LOG_WARN("Install", "Update found without base game: RomFS will be incomplete");
    }

    const fs::path out = opts.output_dir;
    fs::create_directories(out);

    // ExeFS: always from the newest program NCA (update ExeFS is complete, not a delta).
    prog.SetStage("Extracting ExeFS");
    {
        const int si = program->FindSection(1);
        auto pfs = si >= 0 ? program->Pfs0Storage(si) : nullptr;
        auto entries = pfs ? ParsePfs0(*pfs) : std::nullopt;
        if (!entries) {
            error = "ExeFS not found or failed to decrypt";
            return false;
        }
        for (auto& e : *entries)
            if (!ExtractFile(*pfs, e.offset, e.size, out / "exefs" / e.name, prog)) {
                error = "Failed writing ExeFS";
                return false;
            }
    }

    StoragePtr romfs;
    if (const int si = program->FindSection(0); si >= 0)
        romfs = program->RomfsStorage(si);
    StoragePtr control_romfs;
    if (control)
        if (const int si = control->nca->FindSection(0); si >= 0)
            control_romfs = control->nca->RomfsStorage(si);

    u64 total = 0;
    if (romfs && opts.extract_romfs)
        total += RomfsBytes(romfs);
    if (control_romfs)
        total += RomfsBytes(control_romfs);
    for (auto* a : aoc)
        if (const int si = a->nca->FindSection(0); si >= 0)
            if (auto r = a->nca->RomfsStorage(si))
                total += RomfsBytes(r);
    prog.bytes_total = total;
    prog.bytes_done = 0;

    if (control_romfs) {
        prog.SetStage("Extracting control data");
        if (!ExtractRomfs(control_romfs, out / "control", prog, error))
            return false;
    }

    u64 romfs_files = 0;
    if (romfs && opts.extract_romfs) {
        prog.SetStage(update ? "Extracting RomFS (applying update)" : "Extracting RomFS");
        if (!ExtractRomfs(romfs, out / "romfs", prog, error, &romfs_files))
            return false;
    }

    std::vector<std::string> aoc_ids;
    for (auto* a : aoc) {
        const int si = a->nca->FindSection(0);
        auto r = si >= 0 ? a->nca->RomfsStorage(si) : nullptr;
        if (!r)
            continue;
        const std::string id = fmt::format("{:016x}", a->nca->ProgramId());
        if (std::find(aoc_ids.begin(), aoc_ids.end(), id) != aoc_ids.end())
            continue; // same DLC dumped twice
        prog.SetStage("Extracting DLC " + id);
        // DLC RomFS images are small; keep the raw image (the game mounts it as a data storage) plus loose files.
        fs::create_directories(out / "aoc");
        if (!ExtractFile(*r, 0, r->Size(), out / "aoc" / (id + ".romfs"), prog) ||
            !ExtractRomfs(r, out / "aoc" / id, prog, error))
            return false;
        aoc_ids.push_back(id);
    }

    std::ofstream info(out / "install.json");
    info << "{\n  \"program_id\": \"" << fmt::format("{:016x}", program->ProgramId()) << "\",\n"
         << "  \"has_base\": " << (base ? "true" : "false") << ",\n"
         << "  \"has_update\": " << (update ? "true" : "false") << ",\n"
         << "  \"romfs_files\": " << romfs_files << ",\n  \"aoc\": [";
    for (size_t i = 0; i < aoc_ids.size(); i++)
        info << (i ? ", " : "") << '"' << aoc_ids[i] << '"';
    info << "]\n}\n";
    prog.SetStage("Done");
    return true;
}

} // namespace loader
