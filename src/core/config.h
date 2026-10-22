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
    int resolution_scale = 1; // internal render scale
    int fps_limit = 60;
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
