// Menu labels in the style of the game's GText_* textures, drawn with Windows fonts (menu_label.cpp).
#pragma once
#include <string>
#include <vector>
#include "common/common.h"

namespace hle::menu {

struct LabelStyle {
    int w, h;       // texture size
    int cap;        // capital height in pixels
    int left, top;  // ink left margin, capital line
    bool selected;  // yellow gradient (the "_on" textures) instead of white
};

// BC3 blocks (linear order) of `text` as a w x h label; empty if the font cannot draw it.
std::vector<u8> RenderLabel(const LabelStyle& style, const std::wstring& text, const wchar_t* face, bool cjk);

// BC3 blocks (linear order) of `text` in the style of the pause menu's SYSTEM submenu labels (GText_option and
// the like: light upright capitals, 560 x 44; white with a dark drop shadow, or yellow when selected), drawn at that
// size and stretched to a w x h texture (a texture of another size shown on that label's pane). Empty if the font
// cannot draw it.
std::vector<u8> RenderSystemLabel(const std::wstring& text, const wchar_t* face, bool cjk, bool selected, int w, int h);

} // namespace hle::menu
