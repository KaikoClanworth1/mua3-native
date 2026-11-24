// The launcher: what MUA3Setup shows when it runs from an installed game (as "MUA3 Launcher.exe" next to mua3.exe).
// A main menu in the style of the game (drawn here: starfield, angled panels, yellow bars, the Exo 2 font; nothing from
// the game itself) with PLAY, MODS, SETTINGS and EXIT. Mouse, keyboard and controller.
//   PLAY      what is installed, launches mua3.exe (the launcher waits minimised) and shows how the game ended.
//   MODS      install mods by dropping an archive (.zip / .7z / .rar, opened with Windows' own tar.exe, or 7-Zip) or a
//             folder; on/off, load order, conflicts, remove (Recycle Bin). The game's own mod code (core/hle/mods) finds
//             their files and writes order.txt.
//   SETTINGS  mua3.ini, grouped like the game's own options pages; update / repair, uninstall, report a problem.
#pragma once
#include <string>

#include "common/common.h"
#include "setup/install_ops.h"

struct ID3D11Device;
struct ID3D11ShaderResourceView;
struct SDL_Window;
struct ImFont;

namespace launcher {

// What the launcher asks the setup to do (the setup's own pages handle these).
enum class Action { None, Quit, UpdateRepair, Uninstall, Report };

struct Host {
    setup::Installed installed;
    ID3D11Device* dev = nullptr;
    SDL_Window* window = nullptr;
    std::string version; // this build ("v0.3.2")
    float ui = 1.0f;     // the display's scale (1 at 100 %)
};

// Adds the launcher's fonts (Exo 2, from this exe's resources) to the ImGui font atlas.
void LoadFonts();
// (Re)reads the install: game files, mods, settings. Call when the launcher is shown.
void Open(Host& host);
// Draws the whole window for one frame.
Action Frame(Host& host);
// A file or folder dropped on the window (installs it as a mod).
void Drop(const fs::path& path);
// True while the game runs (the main loop can sleep).
bool GameRunning();
void Shutdown();

// Developer tests and screenshots: 0 PLAY, 1 MODS, 2 SETTINGS, 3 EXIT; `focus` puts the keyboard in that section.
void Select(int section, bool focus);
// Tests and screenshots: SETTINGS > RUN DIAGNOSTICS; with `sample_log` (a game log), its result as if a run made it.
void OpenDiagnostics(const fs::path& sample_log);
// Installs `path` as a mod and waits for it (developer test); returns the status line.
std::string InstallNow(const fs::path& path);
// Developer tests: PLAY as if pressed (true when the game started); the MODS master switch flipped and saved.
bool TestPlay();
void TestToggleMods();
// The SETTINGS rows shown, top to bottom (dev=1 hides UPDATE OR REPAIR and UNINSTALL), for test logs.
std::string SettingsRows();

} // namespace launcher
