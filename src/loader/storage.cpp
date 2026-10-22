#include "loader/storage.h"

#include <algorithm>
#include <windows.h>

namespace loader {

std::shared_ptr<FileStorage> FileStorage::Open(const fs::path& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return nullptr;
    LARGE_INTEGER sz{};
    GetFileSizeEx(h, &sz);
    auto s = std::shared_ptr<FileStorage>(new FileStorage());
    s->handle_ = h;
    s->size_ = u64(sz.QuadPart);
    return s;
}

FileStorage::~FileStorage() {
    if (handle_)
        CloseHandle(handle_);
}

void FileStorage::Read(u64 offset, u8* dst, size_t size) const {
    while (size) {
        const DWORD chunk = DWORD(std::min<size_t>(size, 1u << 30));
        OVERLAPPED ov{};
        ov.Offset = DWORD(offset);
        ov.OffsetHigh = DWORD(offset >> 32);
        DWORD got = 0;
        if (!ReadFile(handle_, dst, chunk, &got, &ov) || got != chunk) {
            std::memset(dst + got, 0, chunk - got);
            LOG_ERROR("Storage", "Short read at {:#x} ({} of {})", offset, got, chunk);
        }
        dst += chunk;
        offset += chunk;
        size -= chunk;
    }
}

void MemoryStorage::Read(u64 offset, u8* dst, size_t size) const {
    if (offset >= data_.size()) {
        std::memset(dst, 0, size);
        return;
    }
    const size_t n = std::min<size_t>(size, data_.size() - offset);
    std::memcpy(dst, data_.data() + offset, n);
    if (n < size)
        std::memset(dst + n, 0, size - n);
}

void SubStorage::Read(u64 offset, u8* dst, size_t size) const {
    if (offset + size > size_) {
        const size_t valid = offset < size_ ? size_t(size_ - offset) : 0;
        std::memset(dst + valid, 0, size - valid);
        size = valid;
    }
    if (size)
        parent_->Read(offset_ + offset, dst, size);
}

CtrStorage::CtrStorage(StoragePtr parent, const crypto::Key128& key, std::array<u8, 16> iv, u64 base_offset)
    : parent_(std::move(parent)), aes_(key), iv_(iv), base_offset_(base_offset) {}

std::array<u8, 16> CtrStorage::MakeIv(const std::array<u8, 16>& upper, u64 offset) {
    std::array<u8, 16> iv = upper;
    u64 blk = offset >> 4;
    for (int i = 15; i >= 8; i--) {
        iv[i] = u8(blk & 0xFF);
        blk >>= 8;
    }
    return iv;
}

void CtrStorage::Read(u64 offset, u8* dst, size_t size) const {
    if (!size)
        return;
    const u64 start = common::AlignDown(offset, 16);
    const u64 end = common::AlignUp(offset + size, 16);
    if (start == offset && end == offset + size) {
        parent_->Read(offset, dst, size);
        crypto::AesCtrCrypt(aes_, MakeIv(iv_, base_offset_ + offset), dst, size);
        return;
    }
    std::vector<u8> tmp(end - start);
    parent_->Read(start, tmp.data(), tmp.size());
    crypto::AesCtrCrypt(aes_, MakeIv(iv_, base_offset_ + start), tmp.data(), tmp.size());
    std::memcpy(dst, tmp.data() + (offset - start), size);
}

BktrStorage::BktrStorage(StoragePtr base, StoragePtr patch_raw, const crypto::Key128& key,
                         std::array<u8, 16> upper_iv, u64 section_abs_offset, std::vector<Relocation> relocs,
                         u64 virt_size, std::vector<Subsection> subs, u64 subs_end)
    : base_(std::move(base)), patch_raw_(std::move(patch_raw)), aes_(key), upper_iv_(upper_iv),
      section_abs_offset_(section_abs_offset), relocs_(std::move(relocs)), virt_size_(virt_size),
      subs_(std::move(subs)), subs_end_(subs_end) {}

void BktrStorage::ReadPatch(u64 offset, u8* dst, size_t size) const {
    while (size) {
        auto it = std::upper_bound(subs_.begin(), subs_.end(), offset,
                                   [](u64 o, const Subsection& s) { return o < s.offset; });
        u64 limit = it == subs_.end() ? std::max(subs_end_, offset + size) : it->offset;
        u32 gen = 0;
        bool have = it != subs_.begin();
        if (have)
            gen = std::prev(it)->generation;
        const size_t n = size_t(std::min<u64>(size, limit - offset));

        std::array<u8, 16> iv = upper_iv_;
        if (have) {
            iv[4] = u8(gen >> 24);
            iv[5] = u8(gen >> 16);
            iv[6] = u8(gen >> 8);
            iv[7] = u8(gen);
        }
        const u64 start = common::AlignDown(offset, 16);
        const u64 end = common::AlignUp(offset + n, 16);
        std::vector<u8> tmp(end - start);
        patch_raw_->Read(start, tmp.data(), tmp.size());
        crypto::AesCtrCrypt(aes_, CtrStorage::MakeIv(iv, section_abs_offset_ + start), tmp.data(), tmp.size());
        std::memcpy(dst, tmp.data() + (offset - start), n);
        dst += n;
        offset += n;
        size -= n;
    }
}

void BktrStorage::Read(u64 offset, u8* dst, size_t size) const {
    while (size) {
        if (offset >= virt_size_) {
            std::memset(dst, 0, size);
            return;
        }
        auto it = std::upper_bound(relocs_.begin(), relocs_.end(), offset,
                                   [](u64 o, const Relocation& r) { return o < r.virt; });
        ASSERT(it != relocs_.begin());
        const Relocation& r = *std::prev(it);
        const u64 next = it == relocs_.end() ? virt_size_ : it->virt;
        const size_t n = size_t(std::min<u64>(size, next - offset));
        const u64 phys = r.phys + (offset - r.virt);
        if (r.index == 0) {
            if (!base_)
                std::memset(dst, 0, n);
            else
                base_->Read(phys, dst, n);
        } else {
            ReadPatch(phys, dst, n);
        }
        dst += n;
        offset += n;
        size -= n;
    }
}

} // namespace loader
