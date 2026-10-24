#pragma once
#include "common/common.h"

namespace core {

enum class AudioBackend { XAudio2, SDL, Null };
enum class InputBackend { XInput, SDL };

// Settings shared between the launcher and the runtime (stored in mua3.ini).
struct Config {
    fs::path game_dir = "game";   // extracted data (exefs/, romfs/, ...)
    fs::path isos_dir = "Isos";
    fs::path keys_dir = "keys";
    fs::path save_dir = "save";

    int width = 1920;
    int height = 1080;
    bool fullscreen = false;
    bool borderless = true;
    bool vsync = true;
    int resolution_scale = 0; // internal render scale; 0 = auto from the output size
    int fps_limit = 60;
    int fps_target = 60; // game frame rate: 30 (original) or 60; higher values are experimental (patched timing)
    int swap_interval = 0; // 0: the game's own (MUA3 asks for 2 = 30 fps); 1 presents every vblank
    bool show_fps = true;
    bool show_debug_overlay = false;

    AudioBackend audio = AudioBackend::XAudio2;
    int volume = 100;
    InputBackend input = InputBackend::XInput;
    bool docked = true;
    int language = 1; // 1 = American English
    int log_level = 2; // 0 trace .. 4 error

    bool Load(const fs::path& ini);
    bool Save(const fs::path& ini) const;
};

} // namespace core
