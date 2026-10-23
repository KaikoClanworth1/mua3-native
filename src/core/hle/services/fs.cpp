// fsp-srv: RomFS (served from the extracted folder through a virtual RomFS image) and host-backed save data.
#include <algorithm>
#include <fstream>
#include <list>
#include <unordered_map>

#include <windows.h>

#include "core/hle/service.h"
#include "core/hle/video_output.h"
#include "core/system.h"

namespace hle {

namespace {

constexpr Result kPathNotFound = 0x202;
constexpr Result kPathAlreadyExists = 0x402;
constexpr Result kOutOfRange = 0xD402; // fs 0xD402-ish: invalid offset

// ---------------------------------------------------------------- host file cache
class HostFileCache {
public:
    HANDLE Get(const fs::path& p) {
        std::lock_guard lk(mutex_);
        auto key = p.native();
        auto it = map_.find(key);
        if (it != map_.end()) {
            lru_.splice(lru_.begin(), lru_, it->second);
            return it->second->second;
        }
        HANDLE h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE)
            return h;
        lru_.emplace_front(key, h);
        map_[key] = lru_.begin();
        if (lru_.size() > 256) {
            CloseHandle(lru_.back().second);
            map_.erase(lru_.back().first);
            lru_.pop_back();
        }
        return h;
    }

private:
    std::mutex mutex_;
    std::list<std::pair<std::wstring, HANDLE>> lru_;
    std::unordered_map<std::wstring, std::list<std::pair<std::wstring, HANDLE>>::iterator> map_;
};

HostFileCache& FileCache() {
    static HostFileCache c;
    return c;
}

bool HostRead(const fs::path& p, u64 off, void* dst, size_t size) {
    HANDLE h = FileCache().Get(p);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    OVERLAPPED ov{};
    ov.Offset = DWORD(off);
    ov.OffsetHigh = DWORD(off >> 32);
    DWORD got = 0;
    if (!ReadFile(h, dst, DWORD(size), &got, &ov) && GetLastError() == ERROR_NOACCESS) {
        // The kernel refuses to write into write-tracked (read-only) guest pages; a host memcpy faults and is
        // handled by the tracking handler instead.
        std::vector<u8> tmp(size);
        got = 0;
        ReadFile(h, tmp.data(), DWORD(size), &got, &ov);
        std::memcpy(dst, tmp.data(), got);
    }
    if (got < size)
        std::memset(static_cast<u8*>(dst) + got, 0, size - got);
    return true;
}

// ---------------------------------------------------------------- virtual RomFS
u32 RomfsHash(u32 parent, const std::string& name) {
    u32 h = parent ^ 123456789u;
    for (unsigned char c : name) {
        h = (h >> 5) | (h << 27);
        h ^= c;
    }
    return h;
}

u32 BucketCount(u32 n) {
    if (n < 3)
        return 3;
    if (n < 19)
        return n | 1;
    while (n % 2 == 0 || n % 3 == 0 || n % 5 == 0 || n % 7 == 0 || n % 11 == 0 || n % 13 == 0 || n % 17 == 0)
        n++;
    return n;
}

class VirtualRomfs {
public:
    bool Build(const fs::path& root) {
        root_ = root;
        struct Dir {
            std::string name;
            u32 parent = 0;
            std::vector<u32> dirs, files;
            u32 offset = 0;
        };
        struct File {
            std::string name;
            u32 parent = 0;
            u64 size = 0;
            fs::path host;
            u32 offset = 0;
            u64 data_off = 0;
        };
        std::vector<Dir> dirs(1);
        std::vector<File> files;
        std::vector<std::pair<u32, fs::path>> stack{{0, root}};
        std::error_code ec;
        while (!stack.empty()) {
            auto [di, path] = stack.back();
            stack.pop_back();
            std::vector<fs::directory_entry> entries;
            for (auto& e : fs::directory_iterator(path, ec))
                entries.push_back(e);
            std::sort(entries.begin(), entries.end(),
                      [](auto& a, auto& b) { return a.path().filename() < b.path().filename(); });
            for (auto& e : entries) {
                const std::string name = e.path().filename().string();
                if (e.is_directory()) {
                    const u32 ni = u32(dirs.size());
                    dirs.push_back({name, di});
                    dirs[di].dirs.push_back(ni);
                    stack.emplace_back(ni, e.path());
                } else {
                    const u32 fi = u32(files.size());
                    files.push_back({name, di, e.file_size(), e.path()});
                    dirs[di].files.push_back(fi);
                }
            }
        }
        if (files.empty())
            return false;
        auto entry_size = [](const std::string& n, size_t base) { return u32(common::AlignUp(base + n.size(), 4)); };
        u32 off = 0;
        for (auto& d : dirs) {
            d.offset = off;
            off += entry_size(d.name, 0x18);
        }
        const u32 dir_meta_size = off;
        off = 0;
        u64 data = 0;
        for (auto& f : files) {
            f.offset = off;
            off += entry_size(f.name, 0x20);
            data = common::AlignUp(data, 0x10);
            f.data_off = data;
            data += f.size;
        }
        const u32 file_meta_size = off;
        const u32 ndb = BucketCount(u32(dirs.size())), nfb = BucketCount(u32(files.size()));
        std::vector<u32> dir_hash(ndb, 0xFFFFFFFF), file_hash(nfb, 0xFFFFFFFF);
        std::vector<u8> dir_meta(dir_meta_size), file_meta(file_meta_size);
        auto put32 = [](std::vector<u8>& v, size_t o, u32 x) { std::memcpy(&v[o], &x, 4); };
        auto put64 = [](std::vector<u8>& v, size_t o, u64 x) { std::memcpy(&v[o], &x, 8); };
        constexpr u32 kNone = 0xFFFFFFFF;
        for (size_t i = 0; i < dirs.size(); i++) {
            auto& d = dirs[i];
            const size_t o = d.offset;
            const u32 parent_off = dirs[d.parent].offset;
            put32(dir_meta, o, parent_off);
            // sibling: next dir in parent's list
            u32 sib = kNone;
            if (i != 0) {
                auto& sibs = dirs[d.parent].dirs;
                auto it = std::find(sibs.begin(), sibs.end(), u32(i));
                if (it + 1 != sibs.end())
                    sib = dirs[*(it + 1)].offset;
            }
            put32(dir_meta, o + 4, sib);
            put32(dir_meta, o + 8, d.dirs.empty() ? kNone : dirs[d.dirs[0]].offset);
            put32(dir_meta, o + 12, d.files.empty() ? kNone : files[d.files[0]].offset);
            const u32 b = RomfsHash(i == 0 ? 0 : parent_off, d.name) % ndb;
            put32(dir_meta, o + 16, dir_hash[b]);
            dir_hash[b] = d.offset;
            put32(dir_meta, o + 20, u32(d.name.size()));
            std::memcpy(&dir_meta[o + 24], d.name.data(), d.name.size());
        }
        for (size_t i = 0; i < files.size(); i++) {
            auto& f = files[i];
            const size_t o = f.offset;
            const u32 parent_off = dirs[f.parent].offset;
            put32(file_meta, o, parent_off);
            auto& sibs = dirs[f.parent].files;
            auto it = std::find(sibs.begin(), sibs.end(), u32(i));
            put32(file_meta, o + 4, it + 1 != sibs.end() ? files[*(it + 1)].offset : kNone);
            put64(file_meta, o + 8, f.data_off);
            put64(file_meta, o + 16, f.size);
            const u32 b = RomfsHash(parent_off, f.name) % nfb;
            put32(file_meta, o + 24, file_hash[b]);
            file_hash[b] = f.offset;
            put32(file_meta, o + 28, u32(f.name.size()));
            std::memcpy(&file_meta[o + 32], f.name.data(), f.name.size());
        }
        // Layout: header | dir hash | dir meta | file hash | file meta | (align 0x200) data
        u64 pos = 0x200;
        const u64 dh_off = pos;
        pos += ndb * 4;
        const u64 dm_off = pos;
        pos += dir_meta_size;
        const u64 fh_off = pos;
        pos += nfb * 4;
        const u64 fm_off = pos;
        pos += file_meta_size;
        data_off_ = common::AlignUp(pos, 0x200);
        meta_.assign(size_t(data_off_), 0);
        u64 hdr[10] = {0x50, dh_off, ndb * 4ull, dm_off, dir_meta_size, fh_off, nfb * 4ull, fm_off, file_meta_size,
                       data_off_};
        std::memcpy(meta_.data(), hdr, sizeof(hdr));
        std::memcpy(&meta_[dh_off], dir_hash.data(), ndb * 4);
        std::memcpy(&meta_[dm_off], dir_meta.data(), dir_meta_size);
        std::memcpy(&meta_[fh_off], file_hash.data(), nfb * 4);
        std::memcpy(&meta_[fm_off], file_meta.data(), file_meta_size);
        for (auto& f : files)
            extents_.emplace_back(f.data_off, f.size, std::move(f.host));
        size_ = data_off_ + data;
        LOG_INFO("fs", "Virtual RomFS: {} dirs, {} files, {:.2f} GB", dirs.size(), files.size(),
                 double(size_) / (1 << 30));
        return true;
    }

    u64 Size() const { return size_; }

    void Read(u64 off, u8* dst, size_t size) const {
        while (size) {
            if (off < data_off_) {
                const size_t n = size_t(std::min<u64>(size, data_off_ - off));
                std::memcpy(dst, &meta_[size_t(off)], n);
                dst += n, off += n, size -= n;
                continue;
            }
            const u64 rel = off - data_off_;
            auto it = std::upper_bound(extents_.begin(), extents_.end(), rel,
                                       [](u64 v, const Extent& e) { return v < e.off; });
            if (it == extents_.begin()) {
                std::memset(dst, 0, size);
                return;
            }
            const Extent& e = *std::prev(it);
            if (rel == e.off && e.host.extension() == ".mp4")
                video::OnMovieRead(e.host, 0);
            if (!e.logged.exchange(true))
                LOG_INFO("romfs", "open {} ({} bytes)", e.host.lexically_relative(root_).generic_string(), e.size);
            if (rel >= e.off + e.size) { // padding between files
                const u64 next = it == extents_.end() ? rel + size : it->off;
                const size_t n = size_t(std::min<u64>(size, next - rel));
                std::memset(dst, 0, n);
                dst += n, off += n, size -= n;
                continue;
            }
            const size_t n = size_t(std::min<u64>(size, e.off + e.size - rel));
            HostRead(e.host, rel - e.off, dst, n);
            dst += n, off += n, size -= n;
        }
    }

private:
    struct Extent {
        u64 off, size;
        fs::path host;
        mutable std::atomic<bool> logged{false};
        Extent(u64 o, u64 s, fs::path h) : off(o), size(s), host(std::move(h)) {}
        Extent(Extent&& x) noexcept : off(x.off), size(x.size), host(std::move(x.host)) {}
    };
    fs::path root_;
    std::vector<u8> meta_;
    std::vector<Extent> extents_;
    u64 data_off_ = 0, size_ = 0;
};

std::shared_ptr<VirtualRomfs> GetRomfs() {
    static std::shared_ptr<VirtualRomfs> romfs = [] {
        auto r = std::make_shared<VirtualRomfs>();
        if (!r->Build(core::Sys().Cfg().game_dir / "romfs"))
            LOG_ERROR("fs", "RomFS folder is empty: {}", (core::Sys().Cfg().game_dir / "romfs").string());
        return r;
    }();
    return romfs;
}

class RomfsStorage final : public Service {
public:
    explicit RomfsStorage(std::shared_ptr<VirtualRomfs> r) : Service("fs:IStorage(romfs)"), romfs_(std::move(r)) {
        quiet_ = true;
        Reg(0, "Read", [this](Request& r) {
            const u64 off = r.ArgAt<u64>(0), size = r.ArgAt<u64>(8);
            const IpcBuffer out = r.OutBuffer();
            const u64 n = std::min(size, out.size);
            if (n)
                romfs_->Read(off, r.mem.Ptr(out.addr), size_t(n));
        });
        Reg(4, "GetSize", [this](Request& r) { r.Push<u64>(romfs_->Size()); });
    }

private:
    std::shared_ptr<VirtualRomfs> romfs_;
};

class MemoryStorageService final : public Service {
public:
    explicit MemoryStorageService(std::vector<u8> d) : Service("fs:IStorage(memory)"), data_(std::move(d)) {
        Reg(0, "Read", [this](Request& r) {
            const u64 off = r.ArgAt<u64>(0), size = r.ArgAt<u64>(8);
            if (off >= data_.size())
                return;
            r.WriteOut(data_.data() + off, size_t(std::min<u64>(size, data_.size() - off)));
        });
        Reg(4, "GetSize", [this](Request& r) { r.Push<u64>(data_.size()); });
    }

private:
    std::vector<u8> data_;
};

// ---------------------------------------------------------------- host-backed filesystem (save data)
fs::path Sanitize(const fs::path& root, const std::string& guest) {
    std::string p = guest;
    while (!p.empty() && (p.front() == '/' || p.front() == '\\'))
        p.erase(p.begin());
    fs::path out = root;
    size_t start = 0;
    while (start <= p.size()) {
        size_t e = p.find('/', start);
        if (e == std::string::npos)
            e = p.size();
        const std::string part = p.substr(start, e - start);
        if (!part.empty() && part != "." && part != "..")
            out /= fs::u8path(part);
        start = e + 1;
    }
    return out;
}

class HostFile final : public Service {
public:
    HostFile(fs::path p, u32 mode) : Service("fs:IFile"), path_(std::move(p)), mode_(mode) {
        quiet_ = true;
        Reg(0, "Read", [this](Request& r) {
            const u64 off = r.ArgAt<u64>(8), size = r.ArgAt<u64>(16);
            std::ifstream f(path_, std::ios::binary);
            f.seekg(0, std::ios::end);
            const u64 fsize = u64(f.tellg());
            u64 n = off < fsize ? std::min(size, fsize - off) : 0;
            n = std::min<u64>(n, r.OutSize());
            std::vector<u8> buf(static_cast<size_t>(n));
            f.seekg(off);
            f.read(reinterpret_cast<char*>(buf.data()), n);
            r.WriteOut(buf);
            r.Push<u64>(n);
        });
        Reg(1, "Write", [this](Request& r) {
            const u64 off = r.ArgAt<u64>(8), size = r.ArgAt<u64>(16);
            auto in = r.ReadIn(0);
            in.resize(size_t(std::min<u64>(size, in.size())));
            std::fstream f(path_, std::ios::binary | std::ios::in | std::ios::out);
            f.seekp(off);
            f.write(reinterpret_cast<const char*>(in.data()), in.size());
        });
        Ok(2, "Flush");
        Reg(3, "SetSize", [this](Request& r) {
            std::error_code ec;
            fs::resize_file(path_, r.ArgAt<u64>(0), ec);
        });
        Reg(4, "GetSize", [this](Request& r) {
            std::error_code ec;
            r.Push<u64>(fs::file_size(path_, ec));
        });
    }

private:
    fs::path path_;
    u32 mode_;
};

class HostDirectory final : public Service {
public:
    explicit HostDirectory(fs::path p, u32 mode) : Service("fs:IDirectory") {
        std::error_code ec;
        for (auto& e : fs::directory_iterator(p, ec)) {
            const bool dir = e.is_directory();
            if ((dir && !(mode & 1)) || (!dir && !(mode & 2)))
                continue;
            u8 ent[0x310]{};
            const std::string name = e.path().filename().string();
            std::memcpy(ent, name.data(), std::min<size_t>(name.size(), 0x300));
            ent[0x304] = dir ? 0 : 1;
            const s64 size = dir ? 0 : s64(e.file_size());
            std::memcpy(ent + 0x308, &size, 8);
            entries_.insert(entries_.end(), ent, ent + sizeof(ent));
        }
        Reg(0, "Read", [this](Request& r) {
            const size_t total = entries_.size() / 0x310;
            const size_t want = size_t(r.OutSize() / 0x310);
            const size_t n = std::min(want, total - pos_);
            r.WriteOut(entries_.data() + pos_ * 0x310, n * 0x310);
            pos_ += n;
            r.Push<u64>(n);
        });
        Reg(1, "GetEntryCount", [this](Request& r) { r.Push<u64>(entries_.size() / 0x310); });
    }

private:
    std::vector<u8> entries_;
    size_t pos_ = 0;
};

class HostFileSystem final : public Service {
public:
    explicit HostFileSystem(fs::path root) : Service("fs:IFileSystem"), root_(std::move(root)) {
        std::error_code ec;
        fs::create_directories(root_, ec);
        Reg(0, "CreateFile", [this](Request& r) {
            const auto p = Path(r);
            if (fs::exists(p)) {
                r.SetError(kPathAlreadyExists);
                return;
            }
            fs::create_directories(p.parent_path());
            std::ofstream f(p, std::ios::binary);
            std::error_code e;
            fs::resize_file(p, r.ArgAt<u64>(8), e);
        });
        Reg(1, "DeleteFile", [this](Request& r) {
            std::error_code e;
            if (!fs::remove(Path(r), e))
                r.SetError(kPathNotFound);
        });
        Reg(2, "CreateDirectory", [this](Request& r) {
            const auto p = Path(r);
            if (fs::exists(p)) {
                r.SetError(kPathAlreadyExists);
                return;
            }
            fs::create_directories(p);
        });
        Reg(3, "DeleteDirectory", [this](Request& r) {
            std::error_code e;
            fs::remove(Path(r), e);
        });
        Reg(4, "DeleteDirectoryRecursively", [this](Request& r) {
            std::error_code e;
            fs::remove_all(Path(r), e);
        });
        Reg(5, "RenameFile", [this](Request& r) {
            std::error_code e;
            fs::rename(Sanitize(root_, r.ReadInString(0)), Sanitize(root_, r.ReadInString(1)), e);
        });
        Reg(6, "RenameDirectory", [this](Request& r) {
            std::error_code e;
            fs::rename(Sanitize(root_, r.ReadInString(0)), Sanitize(root_, r.ReadInString(1)), e);
        });
        Reg(7, "GetEntryType", [this](Request& r) {
            const auto p = Path(r);
            if (!fs::exists(p)) {
                r.SetError(kPathNotFound);
                return;
            }
            r.Push<u32>(fs::is_directory(p) ? 0 : 1);
        });
        Reg(8, "OpenFile", [this](Request& r) {
            const auto p = Path(r);
            if (!fs::is_regular_file(p)) {
                r.SetError(kPathNotFound);
                return;
            }
            r.PushInterface(Make<HostFile>(p, r.ArgAt<u32>(0)));
        });
        Reg(9, "OpenDirectory", [this](Request& r) {
            const auto p = Path(r);
            if (!fs::is_directory(p)) {
                r.SetError(kPathNotFound);
                return;
            }
            r.PushInterface(Make<HostDirectory>(p, r.ArgAt<u32>(0)));
        });
        Ok(10, "Commit");
        Reg(11, "GetFreeSpaceSize", [](Request& r) { r.Push<u64>(0x100000000ull); });
        Reg(12, "GetTotalSpaceSize", [](Request& r) { r.Push<u64>(0x100000000ull); });
        Reg(13, "CleanDirectoryRecursively", [this](Request& r) {
            std::error_code e;
            for (auto& x : fs::directory_iterator(Path(r), e))
                fs::remove_all(x.path(), e);
        });
        Reg(14, "GetFileTimeStampRaw", [](Request& r) {
            u8 ts[0x20]{};
            r.PushRaw(ts, sizeof(ts));
        });
    }

private:
    fs::path Path(Request& r) {
        const std::string g = r.ReadInString(0);
        auto p = Sanitize(root_, g);
        LOG_DEBUG("fs", "  path '{}'", g);
        return p;
    }
    fs::path root_;
};

class IDeviceOperator final : public Service {
public:
    IDeviceOperator() : Service("fs:IDeviceOperator") {
        Reg(0, "IsSdCardInserted", [](Request& r) { r.Push<u8>(0); });
        Reg(200, "IsGameCardInserted", [](Request& r) { r.Push<u8>(0); });
        Reg(202, "GetGameCardHandle", [](Request& r) { r.Push<u32>(1); });
    }
};

class FspSrv final : public Service {
public:
    FspSrv() : Service("fsp-srv") {
        Ok(1, "SetCurrentProcess");
        Reg(18, "OpenSdCardFileSystem", [](Request& r) {
            r.PushInterface(Make<HostFileSystem>(core::Sys().Cfg().save_dir / "sdmc"));
        });
        Ok(22, "CreateSaveDataFileSystem");
        Reg(51, "OpenSaveDataFileSystem", [](Request& r) {
            const u8 space = r.ArgAt<u8>(0);
            const u64 tid = r.ArgAt<u64>(8);
            const u8 type = r.ArgAt<u8>(0x28);
            LOG_INFO("fs", "OpenSaveDataFileSystem space={} type={} tid={:#x}", space, type, tid);
            const char* sub = type == 1 ? "account" : type == 3 ? "device" : type == 5 ? "cache" : "save";
            r.PushInterface(Make<HostFileSystem>(core::Sys().Cfg().save_dir / sub));
        });
        Reg(53, "OpenReadOnlySaveDataFileSystem", [](Request& r) {
            r.PushInterface(Make<HostFileSystem>(core::Sys().Cfg().save_dir / "save"));
        });
        Reg(60, "OpenSaveDataInfoReader", [](Request& r) { r.PushInterface(MakeStub("fs:ISaveDataInfoReader")); });
        Reg(61, "OpenSaveDataInfoReaderBySaveDataSpaceId",
            [](Request& r) { r.PushInterface(MakeStub("fs:ISaveDataInfoReader")); });
        Reg(200, "OpenDataStorageByCurrentProcess", [](Request& r) {
            LOG_INFO("fs", "OpenDataStorageByCurrentProcess");
            r.PushInterface(Make<RomfsStorage>(GetRomfs()));
        });
        Reg(202, "OpenDataStorageByDataId", [](Request& r) {
            const u64 id = r.ArgAt<u64>(8);
            const fs::path img = core::Sys().Cfg().game_dir / "aoc" / fmt::format("{:016x}.romfs", id);
            auto data = common::ReadFile(img);
            if (data.empty()) {
                LOG_WARN("fs", "OpenDataStorageByDataId {:#x} not available", id);
                r.SetError(0x7D402);
                return;
            }
            LOG_INFO("fs", "OpenDataStorageByDataId {:#x} ({} bytes)", id, data.size());
            r.PushInterface(Make<MemoryStorageService>(std::move(data)));
        });
        Reg(203, "OpenPatchDataStorageByCurrentProcess", [](Request& r) {
            r.PushInterface(Make<RomfsStorage>(GetRomfs()));
        });
        Reg(400, "OpenDeviceOperator", [](Request& r) { r.PushInterface(Make<IDeviceOperator>()); });
        Reg(500, "OpenSdCardDetectionEventNotifier",
            [](Request& r) { r.PushInterface(MakeStub("fs:IEventNotifier")); });
        Ok(1003, "DisableAutoSaveDataCreation");
        Ok(1004, "SetGlobalAccessLogMode");
        Reg(1005, "GetGlobalAccessLogMode", [](Request& r) { r.Push<u32>(0); });
        Reg(1006, "OutputAccessLogToSdCard", [](Request& r) {
            LOG_DEBUG("fs", "AccessLog: {}", r.ReadInString(0));
        });
        Ok(1011, "GetProgramIndexForAccessLog");
    }
};

} // namespace

void RegisterFs(ServiceManager& sm) {
    sm.RegisterService("fsp-srv", [] { return Make<FspSrv>(); });
}

} // namespace hle
