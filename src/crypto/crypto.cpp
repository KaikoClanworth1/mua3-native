#include "crypto/crypto.h"

#include <algorithm>
#include <fstream>

#include <windows.h>
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")

namespace crypto {

Aes128::Aes128(const Key128& key) {
    BCRYPT_ALG_HANDLE alg{};
    BCRYPT_KEY_HANDLE k{};
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_AES_ALGORITHM, nullptr, 0) != 0)
        PANIC("BCryptOpenAlgorithmProvider failed");
    BCryptSetProperty(alg, BCRYPT_CHAINING_MODE, (PUCHAR)BCRYPT_CHAIN_MODE_ECB,
                      sizeof(BCRYPT_CHAIN_MODE_ECB), 0);
    if (BCryptGenerateSymmetricKey(alg, &k, nullptr, 0, (PUCHAR)key.data(), 16, 0) != 0)
        PANIC("BCryptGenerateSymmetricKey failed");
    alg_ = alg;
    key_ = k;
}

Aes128::~Aes128() {
    if (key_)
        BCryptDestroyKey((BCRYPT_KEY_HANDLE)key_);
    if (alg_)
        BCryptCloseAlgorithmProvider((BCRYPT_ALG_HANDLE)alg_, 0);
}

void Aes128::EncryptEcb(const u8* in, u8* out, size_t size) const {
    ULONG done = 0;
    BCryptEncrypt((BCRYPT_KEY_HANDLE)key_, (PUCHAR)in, ULONG(size), nullptr, nullptr, 0, out, ULONG(size),
                  &done, 0);
}

void Aes128::DecryptEcb(const u8* in, u8* out, size_t size) const {
    ULONG done = 0;
    BCryptDecrypt((BCRYPT_KEY_HANDLE)key_, (PUCHAR)in, ULONG(size), nullptr, nullptr, 0, out, ULONG(size),
                  &done, 0);
}

static void IncrementBE(std::array<u8, 16>& ctr) {
    for (int i = 15; i >= 0; i--)
        if (++ctr[i] != 0)
            break;
}

void AesCtrCrypt(const Aes128& aes, std::array<u8, 16> iv, u8* data, size_t size) {
    constexpr size_t kChunk = 0x10000;
    std::vector<u8> stream(kChunk);
    size_t pos = 0;
    while (pos < size) {
        const size_t n = std::min(kChunk, common::AlignUp(size - pos, 16));
        for (size_t i = 0; i < n; i += 16) {
            std::memcpy(&stream[i], iv.data(), 16);
            IncrementBE(iv);
        }
        aes.EncryptEcb(stream.data(), stream.data(), n);
        const size_t m = std::min(n, size - pos);
        for (size_t i = 0; i < m; i++)
            data[pos + i] ^= stream[i];
        pos += m;
    }
}

static void GfMul(u8* t) {
    u8 carry = 0;
    for (int i = 0; i < 16; i++) {
        const u8 next = t[i] >> 7;
        t[i] = u8(t[i] << 1 | carry);
        carry = next;
    }
    if (carry)
        t[0] ^= 0x87;
}

void AesXtsDecrypt(const Aes128& data_key, const Aes128& tweak_key, u8* data, size_t size,
                   u64 first_sector, size_t sector_size) {
    std::vector<u8> tweaks(sector_size);
    for (size_t off = 0; off < size; off += sector_size, first_sector++) {
        u8 tweak[16]{};
        u64 s = first_sector;
        for (int i = 15; i >= 8; i--) {
            tweak[i] = u8(s & 0xFF);
            s >>= 8;
        }
        tweak_key.EncryptEcb(tweak, tweak, 16);
        const size_t n = std::min(sector_size, size - off);
        for (size_t i = 0; i < n; i += 16) {
            std::memcpy(&tweaks[i], tweak, 16);
            GfMul(tweak);
        }
        u8* p = data + off;
        for (size_t i = 0; i < n; i++)
            p[i] ^= tweaks[i];
        data_key.DecryptEcb(p, p, n);
        for (size_t i = 0; i < n; i++)
            p[i] ^= tweaks[i];
    }
}

static std::string Trim(std::string s) {
    auto ws = [](unsigned char c) { return std::isspace(c); };
    s.erase(s.begin(), std::find_if_not(s.begin(), s.end(), ws));
    s.erase(std::find_if_not(s.rbegin(), s.rend(), ws).base(), s.end());
    return s;
}

static bool ParseKeyFile(const fs::path& path, std::vector<std::pair<std::string, std::vector<u8>>>& out) {
    std::ifstream f(path);
    if (!f)
        return false;
    std::string line;
    while (std::getline(f, line)) {
        const auto eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        std::string name = Trim(line.substr(0, eq));
        std::transform(name.begin(), name.end(), name.begin(), ::tolower);
        auto value = common::FromHex(Trim(line.substr(eq + 1)));
        if (value)
            out.emplace_back(name, *value);
    }
    return true;
}

bool KeySet::LoadProdKeys(const fs::path& path) {
    std::vector<std::pair<std::string, std::vector<u8>>> entries;
    if (!ParseKeyFile(path, entries))
        return false;
    for (auto& [n, v] : entries)
        keys_[n] = v;
    LOG_INFO("Crypto", "Loaded {} keys from {}", entries.size(), path.string());
    return true;
}

bool KeySet::LoadTitleKeys(const fs::path& path) {
    std::vector<std::pair<std::string, std::vector<u8>>> entries;
    if (!ParseKeyFile(path, entries))
        return false;
    for (auto& [n, v] : entries) {
        auto rid = common::FromHex(n);
        if (!rid || rid->size() != 16 || v.size() != 16)
            continue;
        Key128 r, k;
        std::copy(rid->begin(), rid->end(), r.begin());
        std::copy(v.begin(), v.end(), k.begin());
        title_keys_[r] = k;
    }
    return true;
}

std::optional<Key128> KeySet::Get128(const std::string& name) const {
    auto it = keys_.find(name);
    if (it == keys_.end() || it->second.size() != 16)
        return std::nullopt;
    Key128 k;
    std::copy(it->second.begin(), it->second.end(), k.begin());
    return k;
}

std::optional<Key256> KeySet::Get256(const std::string& name) const {
    auto it = keys_.find(name);
    if (it == keys_.end() || it->second.size() != 32)
        return std::nullopt;
    Key256 k;
    std::copy(it->second.begin(), it->second.end(), k.begin());
    return k;
}

std::optional<Key128> KeySet::GetTitleKey(const Key128& rights_id) const {
    auto it = title_keys_.find(rights_id);
    if (it == title_keys_.end())
        return std::nullopt;
    return it->second;
}

void KeySet::AddTitleKey(const Key128& rights_id, const Key128& key) { title_keys_[rights_id] = key; }

std::optional<Key128> KeySet::KeyAreaKey(u32 type, u32 rev) const {
    static const char* names[] = {"application", "ocean", "system"};
    if (type > 2)
        return std::nullopt;
    return Get128(fmt::format("key_area_key_{}_{:02x}", names[type], rev));
}

std::optional<Key128> KeySet::TitleKek(u32 rev) const { return Get128(fmt::format("titlekek_{:02x}", rev)); }

} // namespace crypto
