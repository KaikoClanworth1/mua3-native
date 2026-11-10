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

} // namespace hle::menu
