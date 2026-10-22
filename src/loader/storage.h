#pragma once
#include <memory>
#include <mutex>
#include "common/common.h"
#include "crypto/crypto.h"

namespace loader {

// Random-access read-only byte source. Implementations must be thread-safe.
class Storage {
public:
    virtual ~Storage() = default;
    virtual u64 Size() const = 0;
    virtual void Read(u64 offset, u8* dst, size_t size) const = 0;

    std::vector<u8> ReadBytes(u64 offset, size_t size) const {
        std::vector<u8> v(size);
        Read(offset, v.data(), size);
        return v;
    }
    template <typename T>
    T ReadObj(u64 offset) const {
        T v;
        Read(offset, reinterpret_cast<u8*>(&v), sizeof(T));
        return v;
    }
};
using StoragePtr = std::shared_ptr<const Storage>;

class FileStorage final : public Storage {
public:
    static std::shared_ptr<FileStorage> Open(const fs::path& path);
    ~FileStorage() override;
    u64 Size() const override { return size_; }
    void Read(u64 offset, u8* dst, size_t size) const override;

private:
    void* handle_ = nullptr;
    u64 size_ = 0;
};

class MemoryStorage final : public Storage {
public:
    explicit MemoryStorage(std::vector<u8> data) : data_(std::move(data)) {}
    u64 Size() const override { return data_.size(); }
    void Read(u64 offset, u8* dst, size_t size) const override;
    const std::vector<u8>& Data() const { return data_; }

private:
    std::vector<u8> data_;
};

class SubStorage final : public Storage {
public:
    SubStorage(StoragePtr parent, u64 offset, u64 size) : parent_(std::move(parent)), offset_(offset), size_(size) {}
    u64 Size() const override { return size_; }
    void Read(u64 offset, u8* dst, size_t size) const override;

private:
    StoragePtr parent_;
    u64 offset_, size_;
};

// AES-CTR decrypting view. `base_offset` is the absolute offset (for the counter) of byte 0.
class CtrStorage final : public Storage {
public:
    CtrStorage(StoragePtr parent, const crypto::Key128& key, std::array<u8, 16> iv, u64 base_offset);
    u64 Size() const override { return parent_->Size(); }
    void Read(u64 offset, u8* dst, size_t size) const override;

    static std::array<u8, 16> MakeIv(const std::array<u8, 16>& upper, u64 offset);

private:
    StoragePtr parent_;
    crypto::Aes128 aes_;
    std::array<u8, 16> iv_;
    u64 base_offset_;
};

// Update-patch storage: BKTR indirect (base/patch) table plus AES-CTR-EX table.
class BktrStorage final : public Storage {
public:
    struct Relocation {
        u64 virt;
        u64 phys;
        u32 index; // 0 = base storage, 1 = patch storage
    };
    struct Subsection {
        u64 offset;
        u32 generation;
    };
    // `patch_raw` is the encrypted patch section; `upper_iv` the section counter.
    BktrStorage(StoragePtr base, StoragePtr patch_raw, const crypto::Key128& key, std::array<u8, 16> upper_iv,
                u64 section_abs_offset, std::vector<Relocation> relocs, u64 virt_size,
                std::vector<Subsection> subs, u64 subs_end);
    u64 Size() const override { return virt_size_; }
    void Read(u64 offset, u8* dst, size_t size) const override;

private:
    void ReadPatch(u64 offset, u8* dst, size_t size) const;

    StoragePtr base_, patch_raw_;
    crypto::Aes128 aes_;
    std::array<u8, 16> upper_iv_;
    u64 section_abs_offset_;
    std::vector<Relocation> relocs_;
    u64 virt_size_;
    std::vector<Subsection> subs_;
    u64 subs_end_;
};

} // namespace loader
