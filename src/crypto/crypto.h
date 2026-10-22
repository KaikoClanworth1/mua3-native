#pragma once
#include <map>
#include "common/common.h"

namespace crypto {

using Key128 = std::array<u8, 16>;
using Key256 = std::array<u8, 32>;

// AES-128 in ECB mode backed by Windows CNG.
class Aes128 {
public:
    explicit Aes128(const Key128& key);
    ~Aes128();
    Aes128(const Aes128&) = delete;
    Aes128& operator=(const Aes128&) = delete;

    void EncryptEcb(const u8* in, u8* out, size_t size) const;
    void DecryptEcb(const u8* in, u8* out, size_t size) const;

private:
    void* alg_ = nullptr;
    void* key_ = nullptr;
};

// AES-128-CTR. `iv` is the full 16-byte big-endian counter for byte offset 0 of `data`.
void AesCtrCrypt(const Aes128& aes, std::array<u8, 16> iv, u8* data, size_t size);

// AES-128-XTS with Nintendo's big-endian sector tweak.
void AesXtsDecrypt(const Aes128& data_key, const Aes128& tweak_key, u8* data, size_t size,
                   u64 first_sector, size_t sector_size);

class KeySet {
public:
    bool LoadProdKeys(const fs::path& path);
    bool LoadTitleKeys(const fs::path& path);

    std::optional<Key128> Get128(const std::string& name) const;
    std::optional<Key256> Get256(const std::string& name) const;
    std::optional<Key128> GetTitleKey(const Key128& rights_id) const; // still encrypted with titlekek
    void AddTitleKey(const Key128& rights_id, const Key128& key);

    std::optional<Key256> HeaderKey() const { return Get256("header_key"); }
    std::optional<Key128> KeyAreaKey(u32 type, u32 rev) const;
    std::optional<Key128> TitleKek(u32 rev) const;

private:
    std::map<std::string, std::vector<u8>> keys_;
    std::map<Key128, Key128> title_keys_;
};

} // namespace crypto
