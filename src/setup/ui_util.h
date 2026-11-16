// Small Windows UI helpers shared by the setup wizard (setup.cpp) and the launcher (launcher.cpp).
#pragma once
#include <string>
#include <utility>
#include <vector>

#include "common/common.h"

struct ID3D11Device;
struct ID3D11ShaderResourceView;
struct HWND__;

namespace ui {

extern HWND__* g_hwnd; // the window, as the owner of file dialogs

struct Texture {
    ID3D11ShaderResourceView* srv = nullptr;
    int w = 0, h = 0;
};
// An image file (PNG, JPEG, BMP, ... through WIC) as a texture; empty on failure.
Texture LoadImageTexture(ID3D11Device* dev, const fs::path& path);
void Release(Texture& t);

// The Windows file / folder picker. `filters`: {description, "*.a;*.b"}.
std::vector<fs::path> FileDialog(bool folder, bool multi, const wchar_t* title,
                                 std::vector<std::pair<std::wstring, std::wstring>> filters, const fs::path& start);

// Moves a file or folder to the Recycle Bin (no prompt). False when it could not.
bool Recycle(const fs::path& p);

} // namespace ui
