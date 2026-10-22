#include "loader/formats.h"

#include <algorithm>

namespace loader {

using common::Read;

// ---------------------------------------------------------------- PFS0
std::optional<std::vector<PfsEntry>> ParsePfs0(const Storage& s) {
    if (s.Size() < 0x10)
        return std::nullopt;
    auto hdr = s.ReadBytes(0, 0x10);
    if (std::memcmp(hdr.data(), "PFS0", 4) != 0)
        return std::nullopt;
    const u32 count = Read<u32>(&hdr[4]);
    const u32 strtab = Read<u32>(&hdr[8]);
    if (count > 0x10000 || strtab > 0x100000)
        return std::nullopt;
    auto table = s.ReadBytes(0x10, size_t(count) * 0x18 + strtab);
    const u64 data_start = 0x10 + u64(count) * 0x18 + strtab;
    const char* names = reinterpret_cast<const char*>(&table[size_t(count) * 0x18]);
    std::vector<PfsEntry> out;
    for (u32 i = 0; i < count; i++) {
        const u8* e = &table[i * 0x18];
        const u32 name_off = Read<u32>(e + 16);
        if (name_off >= strtab)
            return std::nullopt;
        out.push_back({std::string(names + name_off, strnlen(names + name_off, strtab - name_off)),
                       data_start + Read<u64>(e), Read<u64>(e + 8)});
    }
    return out;
}

std::optional<std::vector<PfsEntry>> ParseHfs0(const Storage& s) {
    auto hdr = s.ReadBytes(0, 0x10);
    if (std::memcmp(hdr.data(), "HFS0", 4) != 0)
        return std::nullopt;
    const u32 count = Read<u32>(&hdr[4]);
    const u32 strtab = Read<u32>(&hdr[8]);
    if (count > 0x10000 || strtab > 0x100000)
        return std::nullopt;
    auto table = s.ReadBytes(0x10, size_t(count) * 0x40 + strtab);
    const u64 data_start = 0x10 + u64(count) * 0x40 + strtab;
    const char* names = reinterpret_cast<const char*>(&table[size_t(count) * 0x40]);
    std::vector<PfsEntry> out;
    for (u32 i = 0; i < count; i++) {
        const u8* e = &table[i * 0x40];
        const u32 name_off = Read<u32>(e + 16);
        if (name_off >= strtab)
            return std::nullopt;
        out.push_back({std::string(names + name_off, strnlen(names + name_off, strtab - name_off)),
                       data_start + Read<u64>(e), Read<u64>(e + 8)});
    }
    return out;
}

std::optional<std::vector<PfsEntry>> ParseXciSecure(const StoragePtr& xci, StoragePtr* secure_out) {
    if (xci->Size() < 0x200 || xci->ReadObj<u32>(0x100) != 0x44414548) // "HEAD"
        return std::nullopt;
    const u64 root_off = xci->ReadObj<u64>(0x130);
    const u64 root_size = xci->ReadObj<u64>(0x138);
    auto root = std::make_shared<SubStorage>(xci, root_off, root_size);
    auto parts = ParseHfs0(*root);
    if (!parts)
        return std::nullopt;
    for (auto& p : *parts) {
        if (p.name != "secure")
            continue;
        auto sec = std::make_shared<SubStorage>(root, p.offset, p.size);
        auto files = ParseHfs0(*sec);
        if (files && secure_out)
            *secure_out = sec;
        return files;
    }
    return std::nullopt;
}

StoragePtr OpenPfsFile(const StoragePtr& pfs, const std::vector<PfsEntry>& entries, std::string_view name) {
    for (auto& e : entries)
        if (e.name == name)
            return std::make_shared<SubStorage>(pfs, e.offset, e.size);
    return nullptr;
}

// ---------------------------------------------------------------- Ticket
std::optional<Ticket> ParseTicket(const Storage& s) {
    const u32 sig = s.ReadObj<u32>(0);
    u64 data;
    switch (sig) {
    case 0x10000: data = 0x240; break; // RSA-4096
    case 0x10001: data = 0x140; break; // RSA-2048
    case 0x10002: data = 0x80; break;  // ECDSA
    case 0x10003: data = 0x240; break;
    case 0x10004: data = 0x140; break;
    case 0x10005: data = 0x80; break;
    default: return std::nullopt;
    }
    Ticket t;
    s.Read(data + 0x40, t.title_key.data(), 16);
    s.Read(data + 0x160, t.rights_id.data(), 16);
    return t;
}

// ---------------------------------------------------------------- NCA
static std::array<u8, 16> SectionUpperIv(const std::array<u8, 0x200>& fsh) {
    std::array<u8, 16> iv{};
    const u64 v = Read<u64>(&fsh[0x140]);
    for (int i = 0; i < 8; i++)
        iv[i] = u8(v >> (56 - 8 * i));
    return iv;
}

std::shared_ptr<Nca> Nca::Open(StoragePtr storage, const crypto::KeySet& keys, std::shared_ptr<Nca> base,
                               std::string* error) {
    auto fail = [&](std::string msg) -> std::shared_ptr<Nca> {
        if (error)
            *error = std::move(msg);
        return nullptr;
    };
    auto hk = keys.HeaderKey();
    if (!hk)
        return fail("header_key missing from prod.keys");
    if (storage->Size() < 0xC00)
        return fail("file too small");

    auto raw = storage->ReadBytes(0, 0xC00);
    crypto::Key128 k1, k2;
    std::copy_n(hk->begin(), 16, k1.begin());
    std::copy_n(hk->begin() + 16, 16, k2.begin());
    crypto::Aes128 a1(k1), a2(k2);
    crypto::AesXtsDecrypt(a1, a2, raw.data(), 0xC00, 0, 0x200);
    if (std::memcmp(&raw[0x200], "NCA3", 4) != 0)
        return fail(std::memcmp(&raw[0x200], "NCA2", 4) == 0 ? "NCA2 is not supported"
                                                            : "bad NCA magic (wrong header_key?)");

    auto nca = std::shared_ptr<Nca>(new Nca());
    nca->storage_ = storage;
    nca->base_ = std::move(base);
    std::copy_n(raw.begin(), 0x400, nca->header_.begin());
    const u8* h = nca->header_.data();
    nca->content_type_ = NcaContentType(h[0x205]);
    nca->program_id_ = Read<u64>(h + 0x210);
    std::memcpy(nca->rights_id_.data(), h + 0x230, 16);
    const u8 gen = std::max(h[0x206], h[0x220]);
    nca->key_rev_ = gen ? gen - 1 : 0;

    for (int i = 0; i < 4; i++) {
        const u32 start = Read<u32>(h + 0x240 + i * 0x10);
        const u32 end = Read<u32>(h + 0x244 + i * 0x10);
        auto& sec = nca->sections_[i];
        if (end <= start)
            continue;
        sec.present = true;
        sec.offset = u64(start) * 0x200;
        sec.size = u64(end - start) * 0x200;
        std::copy_n(raw.begin() + 0x400 + i * 0x200, 0x200, sec.header.begin());
        sec.fs_type = sec.header[2];
        sec.hash_type = sec.header[3];
        sec.encryption = sec.header[4];
    }

    if (nca->HasRightsId()) {
        auto tk = keys.GetTitleKey(nca->rights_id_);
        if (!tk)
            return fail("title key for rights id " + common::ToHex(nca->rights_id_) + " not found");
        auto kek = keys.TitleKek(nca->key_rev_);
        if (!kek)
            return fail(fmt::format("titlekek_{:02x} missing from prod.keys", nca->key_rev_));
        crypto::Aes128(*kek).DecryptEcb(tk->data(), nca->ctr_key_.data(), 16);
    } else {
        auto kak = keys.KeyAreaKey(h[0x207], nca->key_rev_);
        if (!kak)
            return fail(fmt::format("key_area_key index {} rev {:02x} missing", h[0x207], nca->key_rev_));
        u8 area[0x40];
        crypto::Aes128(*kak).DecryptEcb(h + 0x300, area, 0x40);
        std::memcpy(nca->ctr_key_.data(), area + 0x20, 16);
    }
    return nca;
}

bool Nca::HasRightsId() const {
    return std::any_of(rights_id_.begin(), rights_id_.end(), [](u8 b) { return b != 0; });
}

bool Nca::IsPatch() const {
    for (auto& s : sections_)
        if (s.present && s.encryption == 4)
            return true;
    return false;
}

int Nca::FindSection(u8 fs_type) const {
    for (int i = 0; i < 4; i++)
        if (sections_[i].present && sections_[i].fs_type == fs_type)
            return i;
    return -1;
}

StoragePtr Nca::SectionStorage(int i) const {
    const Section& sec = sections_[i];
    if (!sec.present)
        return nullptr;
    auto raw = std::make_shared<SubStorage>(storage_, sec.offset, sec.size);
    const auto upper = SectionUpperIv(sec.header);
    switch (sec.encryption) {
    case 1: return raw;
    case 3: return std::make_shared<CtrStorage>(raw, ctr_key_, upper, sec.offset);
    case 4: {
        auto plain = std::make_shared<CtrStorage>(raw, ctr_key_, upper, sec.offset);
        const u8* pi = sec.header.data() + 0x100;
        const u64 ind_off = Read<u64>(pi), ind_size = Read<u64>(pi + 8);
        const u64 ctr_off = Read<u64>(pi + 0x20), ctr_size = Read<u64>(pi + 0x28);
        if (std::memcmp(pi + 0x10, "BKTR", 4) != 0 || std::memcmp(pi + 0x30, "BKTR", 4) != 0) {
            LOG_ERROR("NCA", "Section {} has CTR-EX encryption but no BKTR headers", i);
            return nullptr;
        }
        // Indirect table
        auto ind = plain->ReadBytes(ind_off, size_t(ind_size));
        std::vector<BktrStorage::Relocation> relocs;
        const u32 ind_buckets = Read<u32>(&ind[4]);
        const u64 virt_size = Read<u64>(&ind[8]);
        for (u32 b = 0; b < ind_buckets; b++) {
            const u8* bk = &ind[0x4000 * (b + 1)];
            const u32 n = Read<u32>(bk + 4);
            for (u32 e = 0; e < n; e++) {
                const u8* en = bk + 0x10 + e * 0x14;
                relocs.push_back({Read<u64>(en), Read<u64>(en + 8), Read<u32>(en + 16)});
            }
        }
        // AES-CTR-EX table
        auto ctr = plain->ReadBytes(ctr_off, size_t(ctr_size));
        std::vector<BktrStorage::Subsection> subs;
        const u32 ctr_buckets = Read<u32>(&ctr[4]);
        const u64 subs_end = Read<u64>(&ctr[8]);
        for (u32 b = 0; b < ctr_buckets; b++) {
            const u8* bk = &ctr[0x4000 * (b + 1)];
            const u32 n = Read<u32>(bk + 4);
            for (u32 e = 0; e < n; e++) {
                const u8* en = bk + 0x10 + e * 0x10;
                subs.push_back({Read<u64>(en), Read<u32>(en + 12)});
            }
        }
        StoragePtr base_storage;
        if (base_) {
            const int bi = base_->FindSection(0);
            if (bi >= 0)
                base_storage = base_->SectionStorage(bi);
        }
        if (!base_storage)
            LOG_WARN("NCA", "Patch RomFS opened without base game; base-backed data will read as zeros");
        LOG_INFO("NCA", "BKTR: {} relocations, {} subsections, virtual size {:#x}", relocs.size(), subs.size(),
                 virt_size);
        return std::make_shared<BktrStorage>(base_storage, raw, ctr_key_, upper, sec.offset, std::move(relocs),
                                             virt_size, std::move(subs), subs_end);
    }
    default:
        LOG_ERROR("NCA", "Unsupported section encryption type {}", sec.encryption);
        return nullptr;
    }
}

StoragePtr Nca::Pfs0Storage(int i) const {
    const Section& sec = sections_[i];
    if (!sec.present || sec.fs_type != 1)
        return nullptr;
    auto st = SectionStorage(i);
    if (!st)
        return nullptr;
    const u64 off = Read<u64>(&sec.header[0x40]);
    const u64 size = Read<u64>(&sec.header[0x48]);
    return std::make_shared<SubStorage>(st, off, size);
}

StoragePtr Nca::RomfsStorage(int i) const {
    const Section& sec = sections_[i];
    if (!sec.present || sec.fs_type != 0)
        return nullptr;
    auto st = SectionStorage(i);
    if (!st)
        return nullptr;
    if (std::memcmp(&sec.header[0x8], "IVFC", 4) != 0) {
        LOG_ERROR("NCA", "RomFS section without IVFC header");
        return nullptr;
    }
    const u32 levels = Read<u32>(&sec.header[0x14]);
    const u8* lvl = &sec.header[0x18 + (levels - 2) * 0x18];
    return std::make_shared<SubStorage>(st, Read<u64>(lvl), Read<u64>(lvl + 8));
}

// ---------------------------------------------------------------- RomFS
std::optional<std::vector<RomfsFile>> ParseRomfs(const Storage& s) {
    auto hdr = s.ReadBytes(0, 0x50);
    if (Read<u64>(&hdr[0]) != 0x50)
        return std::nullopt;
    const u64 dir_meta_off = Read<u64>(&hdr[0x18]), dir_meta_size = Read<u64>(&hdr[0x20]);
    const u64 file_meta_off = Read<u64>(&hdr[0x38]), file_meta_size = Read<u64>(&hdr[0x40]);
    const u64 data_off = Read<u64>(&hdr[0x48]);
    auto dirs = s.ReadBytes(dir_meta_off, size_t(dir_meta_size));
    auto files = s.ReadBytes(file_meta_off, size_t(file_meta_size));
    constexpr u32 kNone = 0xFFFFFFFF;

    std::vector<RomfsFile> out;
    std::vector<std::pair<u32, std::string>> stack{{0, ""}};
    while (!stack.empty()) {
        auto [d, prefix] = stack.back();
        stack.pop_back();
        if (d + 0x18 > dirs.size())
            return std::nullopt;
        const u8* de = &dirs[d];
        for (u32 f = Read<u32>(de + 0xC); f != kNone;) {
            if (f + 0x20 > files.size())
                return std::nullopt;
            const u8* fe = &files[f];
            const u32 nlen = Read<u32>(fe + 0x1C);
            std::string name(reinterpret_cast<const char*>(fe + 0x20), nlen);
            out.push_back({prefix + name, data_off + Read<u64>(fe + 8), Read<u64>(fe + 0x10)});
            f = Read<u32>(fe + 4);
        }
        for (u32 c = Read<u32>(de + 8); c != kNone;) {
            const u8* ce = &dirs[c];
            const u32 nlen = Read<u32>(ce + 0x14);
            std::string name(reinterpret_cast<const char*>(ce + 0x18), nlen);
            stack.emplace_back(c, prefix + name + "/");
            c = Read<u32>(ce + 4);
        }
    }
    return out;
}

// ---------------------------------------------------------------- LZ4 / NSO
bool Lz4DecompressBlock(const u8* src, size_t src_size, u8* dst, size_t dst_size) {
    const u8* ip = src;
    const u8* iend = src + src_size;
    u8* op = dst;
    u8* oend = dst + dst_size;
    while (ip < iend) {
        const u8 token = *ip++;
        size_t lit = token >> 4;
        if (lit == 15) {
            u8 b;
            do {
                if (ip >= iend) return false;
                b = *ip++;
                lit += b;
            } while (b == 255);
        }
        if (ip + lit > iend || op + lit > oend)
            return false;
        std::memcpy(op, ip, lit);
        ip += lit;
        op += lit;
        if (ip >= iend)
            break;
        if (ip + 2 > iend)
            return false;
        const size_t off = size_t(ip[0]) | size_t(ip[1]) << 8;
        ip += 2;
        if (off == 0 || op - dst < ptrdiff_t(off))
            return false;
        size_t mlen = token & 15;
        if (mlen == 15) {
            u8 b;
            do {
                if (ip >= iend) return false;
                b = *ip++;
                mlen += b;
            } while (b == 255);
        }
        mlen += 4;
        if (op + mlen > oend)
            return false;
        const u8* m = op - off;
        for (size_t i = 0; i < mlen; i++)
            op[i] = m[i];
        op += mlen;
    }
    return op == oend;
}

std::optional<NsoImage> LoadNso(std::span<const u8> f) {
    if (f.size() < 0x100 || std::memcmp(f.data(), "NSO0", 4) != 0)
        return std::nullopt;
    const u32 flags = Read<u32>(&f[0xC]);
    NsoImage img{};
    struct Seg {
        u32 file_off, mem_off, size, csize;
    } segs[3];
    for (int i = 0; i < 3; i++) {
        segs[i].file_off = Read<u32>(&f[0x10 + i * 0x10]);
        segs[i].mem_off = Read<u32>(&f[0x14 + i * 0x10]);
        segs[i].size = Read<u32>(&f[0x18 + i * 0x10]);
        segs[i].csize = Read<u32>(&f[0x60 + i * 4]);
    }
    img.bss_size = Read<u32>(&f[0x3C]);
    std::memcpy(img.module_id.data(), &f[0x40], 0x20);
    const u64 end = common::AlignUp(u64(segs[2].mem_off) + segs[2].size + img.bss_size, 0x1000);
    img.image.assign(size_t(end), 0);
    for (int i = 0; i < 3; i++) {
        const Seg& s = segs[i];
        const bool compressed = flags & (1u << i);
        const u32 in_size = compressed ? s.csize : s.size;
        if (u64(s.file_off) + in_size > f.size() || u64(s.mem_off) + s.size > img.image.size())
            return std::nullopt;
        if (compressed) {
            if (!Lz4DecompressBlock(&f[s.file_off], in_size, &img.image[s.mem_off], s.size))
                return std::nullopt;
        } else {
            std::memcpy(&img.image[s.mem_off], &f[s.file_off], s.size);
        }
    }
    img.text_off = segs[0].mem_off;
    img.text_size = segs[0].size;
    img.ro_off = segs[1].mem_off;
    img.ro_size = segs[1].size;
    img.data_off = segs[2].mem_off;
    img.data_size = segs[2].size;
    return img;
}

// ---------------------------------------------------------------- NPDM
std::optional<Npdm> ParseNpdm(std::span<const u8> f) {
    if (f.size() < 0x80 || std::memcmp(f.data(), "META", 4) != 0)
        return std::nullopt;
    Npdm n;
    const u8 flags = f[0xC];
    n.is64 = flags & 1;
    n.address_space = (flags >> 1) & 7;
    n.main_priority = f[0xE];
    n.main_core = f[0xF];
    n.main_stack_size = Read<u32>(&f[0x1C]);
    n.name.assign(reinterpret_cast<const char*>(&f[0x20]), strnlen(reinterpret_cast<const char*>(&f[0x20]), 0x10));
    const u32 aci_off = Read<u32>(&f[0x70]);
    if (aci_off + 0x18 <= f.size() && std::memcmp(&f[aci_off], "ACI0", 4) == 0)
        n.program_id = Read<u64>(&f[aci_off + 0x10]);
    return n;
}

} // namespace loader
