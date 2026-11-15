// Debug start in any story chapter: `mua3.exe --chapter <N>` (or `--stage ST0N0`), for testing every stage.
//
// The game's CONTINUE loads the chapter recorded in the save (SCENARIO/ChapterSCDATA/CHAPTER_<N>, its missions,
// then its stage). This mode plays from its own save folder, `debug_save` in the working directory, instead of the
// configured one (the user's save is never opened), and sets the chapter fields of that save before the game starts.
// Choosing CONTINUE on the title screen then starts chapter N at its beginning, with the save's party.
//
// The save fields (this game build; the file is plain, with no checksum): save00 holds two copies of the game data,
// 0x5be00 bytes apart; in each, u16 at +0x4d412 and +0x4d43a = the chapter. savedatainfo (the title screen's
// summary): u16 at +0x16 = the chapter.
//
// Only active with `[debug] stage_select=1` in mua3.ini or MUA3_DEBUG_STAGE=1 in the environment.
#include "app/debug_chapter.h"

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fstream>

namespace app::debug_chapter {
namespace {

constexpr size_t kSaveSize = 755712;
constexpr size_t kCopyStride = 0x5be00;
constexpr size_t kFields[] = {0x4d412, 0x4d43a};
constexpr size_t kInfoChapter = 0x16;
constexpr u8 kHeader[] = {0, 0, 0, 0, 2, 0, 0, 0, 0x90, 0x7c, 0x0b, 0};

u16 Get16(const std::vector<u8>& b, size_t at) { return u16(b[at] | b[at + 1] << 8); }
void Put16(std::vector<u8>& b, size_t at, u16 v) { b[at] = u8(v), b[at + 1] = u8(v >> 8); }

bool WriteFile(const fs::path& p, const std::vector<u8>& b) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(b.data()), std::streamsize(b.size()));
    return bool(f);
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

bool Prepare(core::Config& cfg, int chapter, const fs::path& template_dir) {
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
    for (size_t copy = 0; copy < 2; copy++)
        for (size_t f : kFields)
            Put16(save, f + copy * kCopyStride, u16(chapter));
    Put16(info, kInfoChapter, u16(chapter));
    if (!WriteFile(acct / "save00", save) || !WriteFile(acct / "savedatainfo", info)) {
        LOG_ERROR("Debug", "--chapter {}: cannot write {}", chapter, acct.string());
        return true;
    }
    LOG_INFO("Debug", "--chapter {}: debug save set to chapter {} (was {}); choose CONTINUE on the title screen",
             chapter, chapter, was);
    return true;
}

} // namespace app::debug_chapter
