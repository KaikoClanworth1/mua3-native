// MUA3 Setup: everything the wizard does outside its window. It finds the player's game files and keys, checks
// versions, works out disk space, makes the icon from the player's own game, and adds the shortcuts, the Windows
// "Apps" entry and the uninstaller. Nothing here ships any part of the game: the icon is made on the player's PC
// from their own files.
#pragma once
#include <atomic>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "common/common.h"

namespace setup {

// ---- the player's game files
enum class FileKind { Base, Update, Dlc, OtherGame, Unknown };
enum class Verdict { Supported, WrongVersion, WrongGame, Unknown };

constexpr u32 kSupportedUpdate = 524288; // v524288 = update 4.0.1, the version the port is built for
constexpr const char* kSupportedUpdateName = "4.0.1";

struct GameFile {
    fs::path path;
    FileKind kind = FileKind::Unknown;
    std::string title_id;
    std::optional<u32> version; // from the content meta (needs keys) or the file name
    u64 size = 0;
    Verdict verdict = Verdict::Unknown;
    bool selected = true;
    std::string Describe() const; // "Base game · v0 · 14.2 GB"
    std::string Badge() const;    // "Supported version", "Wrong version", ...
};

// Looks at one .nsp / .xci. With `keys` (prod.keys found), the version comes from the encrypted content meta.
GameFile Probe(const fs::path& path, const fs::path& keys);

// Folders worth looking in for game files: the setup's own Isos folder, Downloads, Desktop, Documents, the emulators'
// game folders. Searched two levels deep, .nsp / .xci only.
std::vector<fs::path> CandidateFolders();
std::vector<fs::path> FindGameFiles(const std::vector<fs::path>& folders, std::atomic<bool>& cancel);

// prod.keys in the usual places (the setup's keys folder, the emulators' key folders). Read in place, never copied.
std::vector<fs::path> FindKeys();

// The game's icon (JPEG) from the control data of the given dumps, or empty.
std::vector<u8> ReadGameIcon(const std::vector<fs::path>& files, const fs::path& keys);

// ---- disk space
struct Space {
    u64 free = 0, total = 0;
    std::string drive; // "F:"
};
Space DriveSpace(const fs::path& p);
u64 InstallBytes(const std::vector<GameFile>& files); // the game folder once installed
constexpr u64 kBuildFilesBytes = 6ull << 30;            // generated C++ and compiled parts while it builds
constexpr u64 kBuildFilesKeptBytes = 2ull << 30;        // the compiled parts kept for repairs and updates
std::string Gb(u64 bytes);

// ---- folders
fs::path SetupDir();          // the folder this exe is in
fs::path LocalDataDir();      // %LOCALAPPDATA%\MUA3Native (or the sandbox, see below)
fs::path DefaultInstallDir(); // e.g. C:\Games\Marvel Ultimate Alliance 3
fs::path DefaultBuildDir();   // LocalDataDir()\build
// Test runs set MUA3_SETUP_SANDBOX=<folder>: shortcuts, the Apps entry name and LocalAppData data go there instead
// of the real desktop / Start menu / registry entry.
const char* Sandbox();

// ---- after the build
bool MakeIco(const fs::path& jpeg, const fs::path& ico); // 256 .. 16 px, PNG images in an .ico
bool CreateShortcut(const fs::path& lnk, const fs::path& target, const fs::path& workdir, const fs::path& icon,
                    const std::wstring& description);
fs::path DesktopShortcut();
fs::path StartMenuShortcut();

// What setup installed and where (setup.ini in the install folder and the Apps entry), so re-running it can repair,
// update or uninstall exactly what it made.
struct Installed {
    fs::path dir;         // install folder
    fs::path build_dir;   // build files
    fs::path source_dir;  // the setup package it was installed from (sdk/)
    fs::path keys;        // prod.keys used
    std::vector<fs::path> files; // the game files used
    int language = 1;
    bool desktop = false, start_menu = false;
};
std::optional<Installed> FindInstalled(); // from the Apps entry
bool SaveInstalled(const Installed& in);   // setup.ini
std::optional<Installed> LoadInstalled(const fs::path& dir);
bool RegisterUninstall(const Installed& in, u64 bytes);
void UnregisterUninstall();
u64 FolderBytes(const fs::path& dir);

// Removes what setup made: its files in the install folder, the game data, the shader cache and logs, the build
// files, the shortcuts and the Apps entry. Saves (save\) and screenshots stay unless keep_saves is false. Returns
// what could not be removed (empty: all gone).
std::vector<std::string> Uninstall(const Installed& in, bool keep_saves);

} // namespace setup
