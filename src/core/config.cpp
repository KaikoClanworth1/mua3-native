#include "core/config.h"

#include <fstream>
#include <algorithm>
#include <map>

namespace core {

static std::string Trim(std::string s) {
    while (!s.empty() && std::isspace((unsigned char)s.back()))
        s.pop_back();
    size_t i = 0;
    while (i < s.size() && std::isspace((unsigned char)s[i]))
        i++;
    return s.substr(i);
}

bool Config::Load(const fs::path& ini) {
    std::ifstream f(ini);
    if (!f)
        return false;
    std::map<std::string, std::string> kv;
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == ';' || line[0] == '#' || line[0] == '[')
            continue;
        const auto eq = line.find('=');
        if (eq != std::string::npos)
            kv[Trim(line.substr(0, eq))] = Trim(line.substr(eq + 1));
    }
    auto get_i = [&](const char* k, int& v) {
        if (kv.count(k))
            v = std::atoi(kv[k].c_str());
    };
    auto get_b = [&](const char* k, bool& v) {
        if (kv.count(k))
            v = kv[k] == "1" || kv[k] == "true";
    };
    auto get_p = [&](const char* k, fs::path& v) {
        if (kv.count(k) && !kv[k].empty())
            v = fs::u8path(kv[k]);
    };
    get_p("game_dir", game_dir);
    get_p("isos_dir", isos_dir);
    get_p("keys_dir", keys_dir);
    get_p("save_dir", save_dir);
    get_i("width", width);
    get_i("height", height);
    get_b("fullscreen", fullscreen);
    get_b("borderless", borderless);
    get_b("vsync", vsync);
    get_i("resolution_scale", resolution_scale);
    get_i("fps_limit", fps_limit);
    get_i("fps_target", fps_target);
    get_i("swap_interval", swap_interval);
    get_i("anti_aliasing", anti_aliasing);
    anti_aliasing = std::clamp(anti_aliasing, 0, 4);
    get_b("show_fps", show_fps);
    get_b("show_debug_overlay", show_debug_overlay);
    int a = int(audio), in = int(input);
    get_i("audio_backend", a);
    get_i("input_backend", in);
    get_i("camera_speed", camera_speed);
    camera_speed = std::clamp(camera_speed, 10, 200);
    audio = AudioBackend(std::clamp(a, 0, 2));
    input = InputBackend(std::clamp(in, 0, 1));
    get_i("volume", volume);
    volume = std::clamp(volume, 0, 100);
    get_b("mute", mute);
    get_b("docked", docked);
    get_i("language", language);
    get_i("log_level", log_level);
    get_b("bug_capture", bug_capture);
    return true;
}

bool Config::Save(const fs::path& ini) const {
    std::ofstream f(ini);
    if (!f)
        return false;
    f << "[paths]\n"
      << "game_dir=" << game_dir.string() << "\n"
      << "isos_dir=" << isos_dir.string() << "\n"
      << "keys_dir=" << keys_dir.string() << "\n"
      << "save_dir=" << save_dir.string() << "\n"
      << "[video]\n"
      << "width=" << width << "\nheight=" << height << "\nfullscreen=" << fullscreen
      << "\nborderless=" << borderless << "\nvsync=" << vsync << "\nresolution_scale=" << resolution_scale
      << "\nfps_limit=" << fps_limit << "\nfps_target=" << fps_target << "\nswap_interval=" << swap_interval << "\nanti_aliasing=" << anti_aliasing<< "\nshow_fps=" << show_fps << "\nshow_debug_overlay=" << show_debug_overlay
      << "\n[audio]\naudio_backend=" << int(audio) << "\nvolume=" << volume << "\nmute=" << mute << "\n[input]\ninput_backend="
      << int(input) << "\ncamera_speed=" << camera_speed << "\n[system]\ndocked=" << docked << "\nlanguage=" << language << "\nlog_level=" << log_level
      << "\n";
    if (bug_capture) // (only written when on: release players never see the section)
        f << "[debug]\nbug_capture=1\n";
    return bool(f);
}

} // namespace core
