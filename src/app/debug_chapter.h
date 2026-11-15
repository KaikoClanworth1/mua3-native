// Debug start in any story chapter (see debug_chapter.cpp).
#pragma once
#include "core/config.h"

namespace app::debug_chapter {

// [debug] stage_select=1 or MUA3_DEBUG_STAGE=1.
bool Enabled(const core::Config& cfg);
// STnn0 -> story chapter nn (1..12), else -1.
int ChapterOfStage(const std::string& stage);
// Switches the game to the debug_save folder and sets its save to `chapter`. `template_dir` (optional) seeds an
// empty debug_save. Returns false (nothing changed) when the mode is off.
bool Prepare(core::Config& cfg, int chapter, const fs::path& template_dir);

} // namespace app::debug_chapter
