// Debug start in any story chapter and checkpoint: `mua3.exe --chapter <N> [--checkpoint <S>:<P>]` (or
// `--stage ST0N0`), for testing every stage.
//
// The game's CONTINUE resumes the save's chapter at the save's checkpoint: it loads SCENARIO/ChapterSCDATA/CHAPTER_<N>
// and that chapter's MISSION_<N>_xx files, then restores the party at the checkpoint (section S = the mission, point
// P within it; DATA/DataSavePoint.bin lists them: S.H.I.E.L.D. save points and the autosave points between them),
// loading the stage areas around it. This mode plays from its own save folder, `debug_save` in the working directory,
// instead of the configured one (the user's save is never opened), and sets those fields of that save before the game
// starts. Choosing CONTINUE on the title screen then starts there, with the save's party.
//
// The save fields (this game build; the file is plain, with no checksum): save00 holds two copies of the game data,
// 0x5be00 bytes apart; in each, u16 at +0x4d412 and +0x4d43a = the chapter, u8 at +0x4d43c = the checkpoint's
// section, u8 at +0x4d43d = its point. savedatainfo (the title screen's summary): u16 at +0x16 = the chapter.
//
// Only active with `[debug] stage_select=1` in mua3.ini or MUA3_DEBUG_STAGE=1 in the environment.
#include "app/debug_chapter.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fstream>

namespace app::debug_chapter {
namespace {

constexpr size_t kSaveSize = 755712;
constexpr size_t kCopyStride = 0x5be00;
constexpr size_t kFields[] = {0x4d412, 0x4d43a};
constexpr size_t kSection = 0x4d43c, kPoint = 0x4d43d;
constexpr size_t kInfoChapter = 0x16;
constexpr u8 kHeader[] = {0, 0, 0, 0, 2, 0, 0, 0, 0x90, 0x7c, 0x0b, 0};

u16 Get16(const std::vector<u8>& b, size_t at) { return u16(b[at] | b[at + 1] << 8); }
void Put16(std::vector<u8>& b, size_t at, u16 v) { b[at] = u8(v), b[at + 1] = u8(v >> 8); }
u32 Get32(const std::vector<u8>& b, size_t at) { return u32(Get16(b, at)) | u32(Get16(b, at + 2)) << 16; }

bool WriteFile(const fs::path& p, const std::vector<u8>& b) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(b.data()), std::streamsize(b.size()));
    return bool(f);
}

// DATA/DataSavePoint.bin: u32 at +4 = count, then 44-byte records: u32 +4 chapter, +8 section, +0x10 point,
// u8 +0x19 = 1 for a S.H.I.E.L.D. save point (0: an autosave point), u8 +0x1b = the save point's number.
std::string DescribeCheckpoint(const fs::path& game_dir, int chapter, int section, int point) {
    const auto d = common::ReadFile(game_dir / "romfs" / "DATA" / "DataSavePoint.bin");
    if (d.size() < 8)
        return "(DataSavePoint.bin not found: not checked)";
    const u32 n = Get32(d, 4);
    for (u32 i = 0; i < n && 8 + (i + 1) * 44 <= d.size(); i++) {
        const size_t e = 8 + size_t(i) * 44;
        if (int(Get32(d, e + 4)) == chapter && int(Get32(d, e + 8)) == section && int(Get32(d, e + 0x10)) == point)
            return d[e + 0x19] ? fmt::format("(record {}: S.H.I.E.L.D. save point {})", i, d[e + 0x1b])
                               : fmt::format("(record {}: autosave point)", i);
    }
    return "(NOT a checkpoint listed in DataSavePoint.bin)";
}

} // namespace

bool Enabled(const core::Config& cfg) {
    const char* env = std::getenv("MUA3_DEBUG_STAGE");
    return cfg.stage_select || (env && std::strcmp(env, "1") == 0);
}

int ChapterOfStage(const std::string& stage) {
    // STnn0 is the first stage of story chapter nn (ST010 = chapter 1 .. ST120 = chapter 12)
    if (stage.size() != 5 || stage.compare(0, 2, "ST") != 0 || stage[4] != '0' || !std::isdigit((u8)stage[2]) ||
        !std::isdigit((u8)stage[3]))
        return -1;
    const int n = (stage[2] - '0') * 10 + (stage[3] - '0');
    return n >= 1 && n <= 12 ? n : -1;
}

bool ParseCheckpoint(const std::string& s, int& section, int& point) {
    const size_t sep = s.find_first_of(":.");
    if (sep == std::string::npos || sep == 0 || sep + 1 >= s.size())
        return false;
    for (size_t i = 0; i < s.size(); i++)
        if (i != sep && !std::isdigit((u8)s[i]))
            return false;
    section = std::atoi(s.substr(0, sep).c_str());
    point = std::atoi(s.substr(sep + 1).c_str());
    return section <= 255 && point <= 255;
}

bool Prepare(core::Config& cfg, int chapter, int section, int point, const fs::path& template_dir) {
    if (!Enabled(cfg)) {
        LOG_WARN("Debug", "--chapter ignored: needs [debug] stage_select=1 in mua3.ini or MUA3_DEBUG_STAGE=1");
        return false;
    }
    if (chapter < 0 || chapter > 999) {
        LOG_ERROR("Debug", "--chapter {}: not a chapter number", chapter);
        return false;
    }
    // From here on the game uses debug_save (Config::Save still writes the user's own save_dir to mua3.ini).
    cfg.user_save_dir = cfg.save_dir;
    cfg.save_dir = "debug_save";
    const fs::path acct = cfg.save_dir / "account";
    std::error_code ec;
    fs::create_directories(acct, ec);
    if (!fs::exists(acct / "save00") && !template_dir.empty()) {
        for (const char* name : {"save00", "savedatainfo"})
            fs::copy_file(template_dir / name, acct / name, fs::copy_options::overwrite_existing, ec);
        LOG_INFO("Debug", "debug save copied from {}", template_dir.string());
    }
    auto save = common::ReadFile(acct / "save00");
    auto info = common::ReadFile(acct / "savedatainfo");
    if (save.empty()) {
        LOG_WARN("Debug", "--chapter {}: no save in {} yet. Start a NEW GAME once in this mode (it autosaves in "
                          "ST010), or pass --save-template <folder with save00 and savedatainfo>",
                 chapter, fs::absolute(acct, ec).string());
        return true;
    }
    if (save.size() != kSaveSize || std::memcmp(save.data(), kHeader, sizeof(kHeader)) != 0 || info.size() < 0x20) {
        LOG_ERROR("Debug", "--chapter {}: {} is not a save layout this mode knows ({} bytes); left unchanged", chapter,
                  (acct / "save00").string(), save.size());
        return true;
    }
    const u16 was = Get16(save, kFields[0]);
    const int was_s = save[kSection], was_p = save[kPoint];
    section = std::max(section, 0), point = std::max(point, 0); // no --checkpoint: the chapter start (0:0)
    for (size_t copy = 0; copy < 2; copy++) {
        for (size_t f : kFields)
            Put16(save, f + copy * kCopyStride, u16(chapter));
        save[kSection + copy * kCopyStride] = u8(section);
        save[kPoint + copy * kCopyStride] = u8(point);
    }
    Put16(info, kInfoChapter, u16(chapter));
    if (!WriteFile(acct / "save00", save) || !WriteFile(acct / "savedatainfo", info)) {
        LOG_ERROR("Debug", "--chapter {}: cannot write {}", chapter, acct.string());
        return true;
    }
    LOG_INFO("Debug", "--chapter {}: debug save set to chapter {} checkpoint {}:{} {} (was {} at {}:{}); choose "
                      "CONTINUE on the title screen",
             chapter, chapter, section, point, DescribeCheckpoint(cfg.game_dir, chapter, section, point), was, was_s,
             was_p);
    return true;
}

} // namespace app::debug_chapter
