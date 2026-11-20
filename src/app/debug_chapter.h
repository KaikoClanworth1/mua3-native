// Debug start in any story chapter and checkpoint (see debug_chapter.cpp).
#pragma once
#include "core/config.h"

namespace app::debug_chapter {

// [debug] stage_select=1 or MUA3_DEBUG_STAGE=1.
bool Enabled(const core::Config& cfg);
// STnn0 -> story chapter nn (1..12), else -1.
int ChapterOfStage(const std::string& stage);
// "S:P" (or "S.P") -> section S, point P (DataSavePoint.bin numbering).
bool ParseCheckpoint(const std::string& s, int& section, int& point);
// Switches the game to the debug_save folder and sets its save to `chapter` at checkpoint section:point (0:0 = the
// chapter start). `template_dir` (optional) seeds an empty debug_save. Returns false (nothing changed) when the mode
// is off.
bool Prepare(core::Config& cfg, int chapter, int section, int point, const fs::path& template_dir);
// mua3_cmd.txt `chapter <N> [S:P]` (the stage sweep's chained mode): sets the running game's debug_save to chapter N at
// checkpoint S:P again, for the next CONTINUE (after QUIT GAME to the title). Only after Prepare switched this run to
// debug_save; never another save folder. Logs "debug jump: ..." (or why it refused).
void Command(const std::string& line);

} // namespace app::debug_chapter
