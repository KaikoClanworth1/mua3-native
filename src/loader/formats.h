#pragma once
#include <functional>
#include "loader/storage.h"

namespace loader {

// ---- PFS0 (NSP container, ExeFS) ----
struct PfsEntry {
    std::string name;
    u64 offset; // absolute, within the PFS0 storage
    u64 size;
};
std::optional<std::vector<PfsEntry>> ParsePfs0(const Storage& s);
std::optional<std::vector<PfsEntry>> ParseHfs0(const Storage& s);
// Returns the files of the XCI "secure" partition (where the NCAs live).
std::optional<std::vector<PfsEntry>> ParseXciSecure(const StoragePtr& xci, StoragePtr* secure_out);
StoragePtr OpenPfsFile(const StoragePtr& pfs, const std::vector<PfsEntry>& entries, std::string_view name);

// ---- Ticket ----
struct Ticket {
    crypto::Key128 rights_id{};
    crypto::Key128 title_key{}; // encrypted with titlekek
};
std::optional<Ticket> ParseTicket(const Storage& s);

// ---- NCA ----
enum class NcaContentType : u8 { Program = 0, Meta = 1, Control = 2, Manual = 3, Data = 4, PublicData = 5 };

class Nca {
public:
    struct Section {
        bool present = false;
        u64 offset = 0, size = 0;
        u8 fs_type = 0, hash_type = 0, encryption = 0;
        std::array<u8, 0x200> header{};
    };

    // `base` is the original game's NCA when opening an update's program NCA.
    static std::shared_ptr<Nca> Open(StoragePtr storage, const crypto::KeySet& keys,
                                     std::shared_ptr<Nca> base = nullptr, std::string* error = nullptr);

    NcaContentType ContentType() const { return content_type_; }
    u64 ProgramId() const { return program_id_; }
    bool HasRightsId() const;
    const crypto::Key128& RightsId() const { return rights_id_; }
    const Section& GetSection(int i) const { return sections_[i]; }

    // Decrypted, whole section (for RomFS of a patch NCA this is the BKTR-merged virtual storage).
    StoragePtr SectionStorage(int i) const;
    // PFS0 inside a section (ExeFS / logo / meta).
    StoragePtr Pfs0Storage(int i) const;
    // RomFS (IVFC level 6) inside a section.
    StoragePtr RomfsStorage(int i) const;
    int FindSection(u8 fs_type) const;
    bool IsPatch() const;

private:
    StoragePtr storage_;
    std::shared_ptr<Nca> base_;
    std::array<u8, 0x400> header_{};
    Section sections_[4]{};
    NcaContentType content_type_{};
    u64 program_id_ = 0;
    crypto::Key128 rights_id_{};
    crypto::Key128 ctr_key_{};
    u32 key_rev_ = 0;
};

// ---- RomFS ----
struct RomfsFile {
    std::string path; // '/' separated, no leading slash
    u64 offset;       // absolute within RomFS storage
    u64 size;
};
std::optional<std::vector<RomfsFile>> ParseRomfs(const Storage& s);

// ---- NSO ----
struct NsoImage {
    std::vector<u8> image; // text|ro|data laid out at their memory offsets, bss included (page aligned)
    u32 text_off, text_size, ro_off, ro_size, data_off, data_size, bss_size;
    std::array<u8, 0x20> module_id;
};
std::optional<NsoImage> LoadNso(std::span<const u8> file);

// ---- NPDM ----
struct Npdm {
    bool is64 = true;
    u8 address_space = 3; // 3 = 39-bit
    u8 main_priority = 44;
    u8 main_core = 0;
    u32 main_stack_size = 0x100000;
    u64 program_id = 0;
    std::string name;
};
std::optional<Npdm> ParseNpdm(std::span<const u8> file);

bool Lz4DecompressBlock(const u8* src, size_t src_size, u8* dst, size_t dst_size);

} // namespace loader
