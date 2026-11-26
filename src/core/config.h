#pragma once
#include <map>
#include <string>

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
    int anti_aliasing = 0; // 0 off, 1 FXAA, 2/3/4 SSAA 2x/3x/4x per axis (x the render scale, capped at 4x); read live
    // Upscaler (read live): 0 off, 1 AMD FSR 1 (the game renders below the output size, EASU upscales the finished frame
    // to it, RCAS sharpens). Replaces SSAA while on; FXAA still runs before the upscale.
    int upscaler = 0; // 0 off, 1 FSR 1, 2 DLSS, 3 FSR 3.1 (FSR 1 where DLSS or FSR 3 is unavailable)
    int upscale_quality = 1; // 0 Ultra Quality (1.3x), 1 Quality (1.5x), 2 Balanced (1.7x), 3 Performance (2.0x)
    int sharpness = 80;      // RCAS, 0-100
    int swap_interval = 0; // 0: the game's own (MUA3 asks for 2 = 30 fps); 1 presents every vblank
    bool show_fps = true;
    bool show_debug_overlay = false;

    AudioBackend audio = AudioBackend::XAudio2;
    int volume = 100;
    bool mute = false; // volume kept, output silent (the in-game MUTE row)
    InputBackend input = InputBackend::XInput;
    int camera_speed = 100; // right stick (camera) sensitivity in percent
    // Face buttons (read live, #14): 0 = by the letter printed on the controller, so the game's A/B/X/Y prompts match an
    // Xbox pad (PlayStation: cross = A, circle = B, square = X, triangle = Y); 1 = by position, like a Switch.
    int button_layout = 0;
    // Stick deadzones in percent of full tilt (read live, #17); the game adds its own small deadzone on top.
    int left_deadzone = 10;
    int right_deadzone = 10;
    // The mouse turns the camera while playing (read live): 0 off, 1 on, 2 on with Y inverted; mouse_sensitivity in
    // percent (100 = 0.0025 radians per mouse count).
    int mouse_camera = 1;
    int mouse_sensitivity = 100;
    bool docked = true;
    int language = 1; // 1 = American English
    int log_level = 2; // 0 trace .. 4 error
    bool bug_capture = false; // [debug] F4 writes a bug capture folder (platform/bug_capture.cpp)
    bool stage_select = false; // [debug] allows --chapter / --stage (app/debug_chapter.cpp)
    std::string fxc_cs;        // [debug] compute shader hashes built by FXC, not the direct backend (gpu/d3d11/shader_cache.cpp)
    std::string skip_cs;       // [debug] compute shader hashes never dispatched (#8: a driver that cannot compile one)
    // [debug] rewritten (video) textures through UpdateSubresource instead of the staging ring, as MUA3_TEX_UPDATESUB=1
    // (#22: a player's test of the grey movie without the ring)
    bool tex_updatesub = false;
    bool mods_enabled = true;  // [mods] enabled: master switch for the mods folder (core/hle/mods.h, docs/mods.md)
    // [keys]: keyboard / mouse bindings that differ from the defaults, action -> "KEY[, MOUSE BUTTON]" (hle::keybinds)
    std::map<std::string, std::string> keys;
    // Not stored: set while --chapter plays from its own save folder; Save() writes this as save_dir instead.
    fs::path user_save_dir;

    bool Load(const fs::path& ini);
    bool Save(const fs::path& ini) const;
};

} // namespace core
