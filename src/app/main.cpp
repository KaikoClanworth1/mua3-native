// MUA3 runtime entry point: window/input/audio on the main thread, the game on its own threads.
#include <atomic>
#include <algorithm>
#include <csignal>
#include <exception>
#include <fstream>
#include <map>
#include <sstream>
#include <chrono>
#include <thread>

#include <SDL3/SDL.h>
#include <imgui.h>
#include <windows.h>
#include <psapi.h>
#include <timeapi.h>
#include <dbghelp.h>

#include "common/crash.h"
#include "common/profiler.h"
#include "core/config.h"
#include "core/kernel/kernel.h"
#include "core/system.h"
#include "core/hle/audio_output.h"
#include "core/hle/camera_speed.h"
#include "core/hle/hid.h"
#include "core/hle/keybinds.h"
#include "core/hle/menu_graphics.h"
#include "core/hle/menu_mods.h"
#include "gpu/d3d11/renderer_d3d11.h"
#include "app/debug_chapter.h"
#include "platform/bug_capture.h"
#include "gpu/d3d11/shader_cache.h"
#include "gpu/d3d11/texture_stats.h"
#include "core/cpu/cpu.h"
#include "gpu/shader/harvest.h"
#include "gpu/gpu.h"
#include "gpu/nvn_encoder.h"
#include "platform/platform.h"

namespace {

gpu::D3D11Renderer* g_renderer = nullptr;
// "Graphics device was reset" toast after a device-lost recovery (platform::Display::RecoverDevice), until then.
std::chrono::steady_clock::time_point g_device_toast_until{};
platform::Display* g_display = nullptr;
std::atomic<bool> g_script_active{false};

struct Stats {
    std::atomic<u64> presents{0};
    std::atomic<u32> width{0}, height{0};
    double fps = 0;
    u64 last_presents = 0;
    std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
} g_stats;

// Overlay scale on high-DPI monitors (4K at 150-225%): set from the window's display at startup.
float g_ui = 1.0f;
ImVec2 UiVec(float x, float y) { return ImVec2(x * g_ui, y * g_ui); }

// Settings changed in the game's GRAPHICS page (core::System::UpdateConfig): the window mode and size, and the
// overlay's FPS counter. The frame limiter reads the running config itself.
// Hidden test instances keep their window as it is.
void ApplyGameSettings(const core::Config& n, core::Config& cfg, platform::Display& display, platform::Input& input,
                       bool keep_window) {
    cfg.show_fps = n.show_fps;
    cfg.fps_limit = n.fps_limit;
    cfg.vsync = n.vsync;
    if ((n.volume != cfg.volume || n.mute != cfg.mute) && !keep_window) // test instances stay muted
        platform::Audio().SetVolume(n.mute ? 0 : n.volume);
    cfg.volume = n.volume;
    cfg.mute = n.mute;
    if (n.camera_speed != cfg.camera_speed)
        hle::camera_speed::Set(n.camera_speed);
    cfg.camera_speed = n.camera_speed;
    input.SetButtonLayout(n.button_layout);
    cfg.button_layout = n.button_layout;
    input.SetDeadzones(n.left_deadzone, n.right_deadzone);
    cfg.left_deadzone = n.left_deadzone;
    cfg.right_deadzone = n.right_deadzone;
    input.SetMouseCamera(n.mouse_camera, n.mouse_sensitivity);
    cfg.mouse_camera = n.mouse_camera;
    cfg.mouse_sensitivity = n.mouse_sensitivity;
    LOG_INFO("Main", "settings from the game: {} {}x{}, fps limit {}, show fps {}", n.fullscreen ? "fullscreen" : "window",
             n.width, n.height, n.fps_limit, n.show_fps);
    if (keep_window)
        return;
    SDL_Window* w = display.Window();
    const bool fullscreen = (SDL_GetWindowFlags(w) & SDL_WINDOW_FULLSCREEN) != 0;
    if (n.fullscreen != fullscreen)
        display.ToggleFullscreen();
    if (!n.fullscreen && (n.width != cfg.width || n.height != cfg.height || n.fullscreen != cfg.fullscreen)) {
        SDL_SetWindowSize(w, n.width, n.height);
        SDL_SetWindowPosition(w, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    }
    cfg.width = n.width;
    cfg.height = n.height;
    cfg.fullscreen = n.fullscreen;
}

void DrawOverlay(const core::Config& cfg, platform::Input& input, bool& show) {
    if (ImGui::IsKeyPressed(ImGuiKey_F1))
        show = !show;
    const auto now = std::chrono::steady_clock::now();
    const double dt = std::chrono::duration<double>(now - g_stats.t0).count();
    if (dt >= 0.5) {
        const u64 p = g_stats.presents;
        g_stats.fps = (p - g_stats.last_presents) / dt;
        g_stats.last_presents = p;
        g_stats.t0 = now;
    }
    if (std::chrono::steady_clock::now() < g_device_toast_until) {
        const ImVec2 ds = ImGui::GetIO().DisplaySize;
        ImGui::SetNextWindowPos(ImVec2(ds.x * 0.5f, ds.y - 24 * g_ui), 0, ImVec2(0.5f, 1.0f));
        ImGui::SetNextWindowBgAlpha(0.6f);
        ImGui::Begin("##devicereset", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                         ImGuiWindowFlags_NoNav);
        ImGui::Text("Graphics device was reset");
        ImGui::End();
    }
    if (cfg.show_fps) {
        ImGui::SetNextWindowPos(UiVec(8, 8));
        ImGui::SetNextWindowBgAlpha(0.35f);
        ImGui::Begin("##fps", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                         ImGuiWindowFlags_NoNav);
        ImGui::Text("%.1f FPS", g_stats.fps);
        ImGui::End();
    }
    if (g_renderer && g_renderer->Shaders()) {
        // Like a PC game's first-launch shader compilation: everything the game ships is compiled up front.
        // Before the game shows its first frame this is the whole screen; afterwards a corner box. On the title menu
        // (and the boot screens before it) with the launch's shaders still compiling, a note says what that means.
        auto* sc = g_renderer->Shaders();
        const u32 pending = sc->pending.load(), total = std::max(sc->prewarm_total.load(), pending);
        const bool prewarming = !sc->PrewarmDone() && pending > 0;
        // the title menu, or the boot and PRESS + screens before it (at most 5 minutes, in case the hook never fires)
        static bool seen_title = false;
        static auto first_frame = std::chrono::steady_clock::time_point{};
        if (g_stats.presents > 0 && first_frame == std::chrono::steady_clock::time_point{})
            first_frame = now;
        const bool at_title = g_stats.presents > 0 && hle::menu::TitleMenuShowing();
        seen_title |= at_title;
        const bool title =
            at_title || (g_stats.presents > 0 && !seen_title && now - first_frame < std::chrono::minutes(5));
        static bool warned = false; // the title note was shown this launch
        static auto ready_until = std::chrono::steady_clock::time_point{};
        const float frac = total ? float(total - pending) / float(total) : 0.0f;
        const ImVec2 ds = ImGui::GetIO().DisplaySize;
        const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                       ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav;
        char label[64];
        std::snprintf(label, sizeof(label), "%u / %u", total - pending, total);
        if (warned && !prewarming && title && ready_until == std::chrono::steady_clock::time_point{})
            ready_until = now + std::chrono::seconds(4);
        if (pending > 0 && g_stats.presents == 0) {
            ImGui::SetNextWindowPos(ImVec2(ds.x * 0.5f, ds.y * 0.5f), 0, ImVec2(0.5f, 0.5f));
            ImGui::SetNextWindowBgAlpha(0.9f);
            ImGui::Begin("##shaders", nullptr, flags);
            ImGui::Text("Preparing shaders for your GPU");
            ImGui::ProgressBar(frac, UiVec(520, 24), label);
            ImGui::TextDisabled("Compiled once and kept; the game continues loading meanwhile.");
            ImGui::End();
        } else if (prewarming && title) {
            // (bottom centre: the title menu's items are on the left, its button guide along the bottom edge)
            warned = true;
            ImGui::SetNextWindowPos(ImVec2(ds.x * 0.5f, ds.y - 64 * g_ui), 0, ImVec2(0.5f, 1.0f));
            ImGui::SetNextWindowBgAlpha(0.85f);
            ImGui::Begin("##shaderwarn", nullptr, flags);
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "Shaders are still being prepared (%u left)", pending);
            ImGui::Text("The game may stutter or show missing textures until this finishes.");
            ImGui::ProgressBar(frac, UiVec(560, 20), label);
            ImGui::TextDisabled("You can wait here, or start playing now: it keeps going in the background.");
            ImGui::End();
        } else if (now < ready_until && title) {
            ImGui::SetNextWindowPos(ImVec2(ds.x * 0.5f, ds.y - 64 * g_ui), 0, ImVec2(0.5f, 1.0f));
            ImGui::SetNextWindowBgAlpha(0.85f);
            ImGui::Begin("##shaderwarn", nullptr, flags);
            ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 1.0f), "Shaders ready");
            ImGui::End();
        } else if (pending > 0) {
            ImGui::SetNextWindowPos(ImVec2(ds.x - 8 * g_ui, 8 * g_ui), 0, ImVec2(1, 0));
            ImGui::SetNextWindowBgAlpha(0.5f);
            ImGui::Begin("##shaders", nullptr, flags);
            if (prewarming)
                ImGui::Text("Compiling shaders: %u left", pending);
            else
                ImGui::Text("Compiling shaders");
            ImGui::ProgressBar(frac, UiVec(220, 16), label);
            if (prewarming && warned)
                ImGui::TextDisabled("Stutter and missing textures until done");
            ImGui::End();
        }
    }
    if (!show)
        return;
    ImGui::SetNextWindowPos(UiVec(8, 48), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(UiVec(460, 0), ImGuiCond_FirstUseEver);
    ImGui::Begin("MUA3 Native (F1)");
    auto& g = gpu::G();
    ImGui::Text("Game frames presented: %llu (%.1f fps)", (unsigned long long)g_stats.presents.load(), g_stats.fps);
    ImGui::Text("Swapchain: %ux%u", g_stats.width.load(), g_stats.height.load());
    ImGui::Text("GPU submits: %llu   methods: %llu   draws: %llu", (unsigned long long)g.submits.load(),
                (unsigned long long)g.methods.load(), (unsigned long long)g.draws.load());
    ImGui::Text("Input: %s   Audio: %s", input.DeviceName().c_str(), platform::Audio().BackendName().c_str());
    ImGui::Separator();
    if (const float pct = cpu::g_native_percent; pct >= 0)
        ImGui::Text("CPU: %.2f%% of guest code native (statically recompiled x64)", pct);
    else
        ImGui::Text("CPU: statically recompiled x64");
    if (const u32 jit = cpu::g_jit_threads.load(); jit)
        ImGui::Text("     JIT fallback used by %u thread(s) for code with no recompiled version", jit);
    else
        ImGui::Text("     JIT fallback: never used");
    if (g_renderer && g_renderer->Shaders())
        ImGui::Text("GPU: NVN -> native D3D11, %zu shaders (%u compiling)", g_renderer->Shaders()->Count(),
                    g_renderer->Shaders()->pending.load());
    if (ImGui::Button("Dump guest threads to log"))
        core::Sys().Kern().DumpAllThreads();
    ImGui::SameLine();
    if (ImGui::Button("Dump render targets") && g_renderer)
        g_renderer->dump_surfaces_ = true;
    ImGui::End();

    if (g_renderer) {
        static bool show_tex = std::getenv("MUA3_TEXVIEW") != nullptr;
        if (show_tex) {
            ImGui::SetNextWindowPos(UiVec(500, 48), ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowSize(UiVec(760, 640), ImGuiCond_FirstUseEver);
            ImGui::Begin("Textures", &show_tex);
            int n = 0;
            static const u32 min_size = [] {
                const char* m = std::getenv("MUA3_TEXVIEW_MIN");
                return m ? u32(std::atoi(m)) : 0u;
            }();
            for (auto& t : g_renderer->RecentTextures()) {
                if (std::max(t.w, t.h) < min_size)
                    continue;
                const float scale = 96.0f / std::max<u32>(std::max(t.w, t.h), 1);
                ImGui::BeginGroup();
                ImGui::Image((ImTextureID)t.srv.Get(), ImVec2(t.w * scale, t.h * scale));
                ImGui::Text("%ux%u %02x%s", t.w, t.h, t.fmt, t.rt ? " RT" : "");
                ImGui::EndGroup();
                if (++n % 6)
                    ImGui::SameLine();
            }
            ImGui::End();
        }
    }
}

u64 ParseButtons(const std::string& btns);

// Test aid: external control channel. Each line of mua3_cmd.txt is executed then the file is deleted:
//   press <BUTTON>[+BUTTON...] [hold_ms]     tap buttons on player 1
//   xpad <XBOX>[+XBOX...][/...] [hold_ms]    the same through the Xbox pad mapping (A B X Y LB RB LT RT START BACK, D-pad);
//                                            stages split by / are held hold_ms each
//   key <KEY>[+KEY...][/...] [hold_ms]       keys and mouse buttons through the KEY BINDINGS (names as in mua3.ini's
//                                            [keys], _ for spaces: L, NUM_8, MOUSE_LEFT; NONE = nothing held)
//   mouse <dx> <dy> [times]                  mouse counts to the MOUSE CAMERA, every 16 ms (default once)
//   stick <lx> <ly> [hold_ms]                tilt player 1's left stick (rstick: the right stick)
//   shot <path.bmp>                          save the next presented game frame
void PollCommandFile(platform::Input* input) {
    static auto last = std::chrono::steady_clock::now();
    if (std::chrono::steady_clock::now() - last < std::chrono::milliseconds(200))
        return;
    last = std::chrono::steady_clock::now();
    if (!fs::exists("mua3_cmd.txt"))
        return;
    std::ifstream f("mua3_cmd.txt");
    std::vector<std::string> lines;
    for (std::string l; std::getline(f, l);)
        if (!l.empty())
            lines.push_back(l);
    f.close();
    std::error_code ec;
    fs::remove("mua3_cmd.txt", ec);
    std::thread([lines, input] {
        for (auto& line : lines) {
            std::istringstream ss(line);
            std::string cmd, arg;
            int hold = 150;
            ss >> cmd >> arg >> hold;
            if (!hold)
                hold = 150;
            if (cmd == "press") {
                g_script_active = true;
                hle::hid::SetPad(0, hle::hid::PadInput{true, ParseButtons(arg)});
                std::this_thread::sleep_for(std::chrono::milliseconds(hold));
                hle::hid::SetPad(0, hle::hid::PadInput{true});
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
                g_script_active = false;
            } else if (cmd == "xpad" && input) { // xpad <RB/RB+X/...> [hold_ms]: an Xbox pad's buttons, each stage held
                static const std::pair<const char*, u32> names[] = {
                    {"A", 0x1000}, {"B", 0x2000}, {"X", 0x4000}, {"Y", 0x8000}, {"LB", 0x100}, {"RB", 0x200},
                    {"START", 0x10}, {"BACK", 0x20}, {"UP", 0x1}, {"DOWN", 0x2}, {"LEFT", 0x4}, {"RIGHT", 0x8},
                    {"LT", platform::Input::kSimLeftTrigger}, {"RT", platform::Input::kSimRightTrigger}};
                g_script_active = true;
                std::istringstream stages(arg);
                for (std::string stage; std::getline(stages, stage, '/');) {
                    u32 xb = 0;
                    std::istringstream parts(stage);
                    for (std::string n; std::getline(parts, n, '+');)
                        for (auto& [name, bit] : names)
                            if (n == name)
                                xb |= bit;
                    LOG_INFO("Main", "xpad {}: game buttons {:#x}", stage, input->SimulateXInput(xb));
                    std::this_thread::sleep_for(std::chrono::milliseconds(hold));
                }
                input->SimulateXInput(0);
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
                g_script_active = false;
            } else if (cmd == "key" && input) { // key <L/MOUSE_LEFT+ESC/...> [hold_ms]: keys, each stage held
                g_script_active = true;
                std::istringstream stages(arg);
                for (std::string stage; std::getline(stages, stage, '/');) {
                    std::vector<u16> keys;
                    u32 mouse = 0;
                    std::istringstream parts(stage);
                    for (std::string n; std::getline(parts, n, '+');) {
                        std::replace(n.begin(), n.end(), '_', ' ');
                        if (const u8 m = hle::keybinds::MouseFromName(n))
                            mouse |= 1u << (m - 1);
                        else if (const u16 k = hle::keybinds::KeyFromName(n))
                            keys.push_back(k);
                    }
                    LOG_INFO("Main", "key {}: game buttons {:#x}", stage, input->SimulateKeys(keys, mouse));
                    std::this_thread::sleep_for(std::chrono::milliseconds(hold));
                }
                input->SimulateKeys({}, 0);
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
                g_script_active = false;
            } else if (cmd == "mouse") { // mouse <dx> <dy> [times]: counts to the mouse camera
                float dx = 0, dy = 0;
                int times = 1;
                std::string c;
                std::istringstream(line) >> c >> dx >> dy >> times;
                for (int k = 0; k < std::max(times, 1); k++) {
                    hle::camera_speed::AddMouse(dx, dy);
                    std::this_thread::sleep_for(std::chrono::milliseconds(16));
                }
            } else if (cmd == "stick" || cmd == "rstick") { // [r]stick <x> <y> [hold_ms]: tilt the left (right) stick
                int lx = 0, ly = 0, ms = 1000;
                std::string c;
                std::istringstream(line) >> c >> lx >> ly >> ms;
                g_script_active = true;
                hle::hid::PadInput in{true};
                (cmd == "stick" ? in.lx : in.rx) = lx;
                (cmd == "stick" ? in.ly : in.ry) = ly;
                hle::hid::SetPad(0, in);
                std::this_thread::sleep_for(std::chrono::milliseconds(ms));
                hle::hid::SetPad(0, hle::hid::PadInput{true});
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
                g_script_active = false;
            } else if (cmd == "shot" && g_renderer) {
                g_renderer->RequestFrameShot(fs::u8path(arg));
            } else if (cmd == "shotui" && g_display) { // the window as shown, overlay included
                g_display->capture_path = fs::u8path(arg);
            } else if (cmd == "chapter") { // chapter <N> [S:P]: the debug save's next CONTINUE (MUA3_DEBUG_STAGE only)
                app::debug_chapter::Command(line);
            } else if (cmd == "crash") { // test aid: an access violation (exercises the crash files and report)
                LOG_WARN("Main", "crash requested by the test channel");
                *static_cast<volatile int*>(nullptr) = 1;
            } else if (cmd == "dump") {
                core::Sys().Kern().DumpAllThreads(arg == "host");
            } else if (cmd == "pixel" && g_renderer) { // pixel <surface gpu va hex> <x> <y>
                std::string c, va;
                u32 x = 0, y = 0;
                std::istringstream(line) >> c >> va >> x >> y;
                g_renderer->ReadPixel(std::strtoull(va.c_str(), nullptr, 16), x, y);
            } else if (cmd == "capture") { // the F4 bug capture (needs [debug] bug_capture=1)
                platform::bugcap::Request();
            } else if (cmd == "dumprt" && g_renderer) {
                g_renderer->dump_surfaces_ = true;
            } else if (cmd == "tracedraws" && g_renderer) {
                std::string rt;
                std::istringstream(line) >> cmd >> arg >> rt;
                g_renderer->trace_rt_ = rt.empty() ? 0 : rt == "all" ? ~0ull : std::strtoull(rt.c_str(), nullptr, 16);
                g_renderer->trace_draws_ = std::atoi(arg.c_str());
            } else if (cmd == "pick" && g_renderer) { // pick <x> <y> in 0..1 screen fractions
                float x = 0, y = 0;
                std::string c;
                std::istringstream(line) >> c >> x >> y;
                g_renderer->Pick(x, y);
            } else if (cmd == "occl" && g_renderer) { // samples written by each draw of the next frame
                g_renderer->CountSamples();
            } else if (cmd == "trace") {
                core::Sys().ArmTraces();
            } else if (cmd == "regs") {
                core::Sys().Kern().DumpThreadRegs(u32(std::strtoul(arg.c_str(), nullptr, 0)));
            } else if (cmd == "gpeek") { // gpeek <gpu va> [count]: GPU memory as u32 + float
                const u64 addr = std::strtoull(arg.c_str(), nullptr, 16);
                const int n = std::min(hold == 150 ? 16 : hold, 1024);
                std::vector<u32> words(size_t(n), 0);
                gpu::G().ReadGpu(addr, words.data(), words.size() * 4);
                std::string out;
                for (int i = 0; i < n; i++) {
                    float f;
                    std::memcpy(&f, &words[size_t(i)], 4);
                    out += fmt::format("{}{:08x} ({:g})", i % 4 ? "  " : fmt::format("\n  +{:04x}: ", i * 4), words[size_t(i)], f);
                }
                LOG_INFO("Main", "gpeek {:#x}:{}", addr, out);
            } else if (cmd == "peek") { // peek <addr> [qwords]: guest memory as symbolized u64s
                const u64 addr = std::strtoull(arg.c_str(), nullptr, 0);
                auto& mem = core::Sys().Mem();
                std::string out;
                for (int i = 0; i < std::min(hold == 150 ? 16 : hold, 256); i++) {
                    const u64 a = addr + u64(i) * 8;
                    if (!mem.IsValid(a, 8))
                        break;
                    const u64 v = mem.Read<u64>(a);
                    out += fmt::format("\n  +{:03x}: {:016x} {}", i * 8, v, v >> 32 ? core::Sys().Symbolize(v) : "");
                }
                LOG_INFO("Main", "peek {:#x}:{}", addr, out);
            } else if (cmd == "wait") {
                std::this_thread::sleep_for(std::chrono::milliseconds(std::atoi(arg.c_str())));
            }
            LOG_INFO("Main", "command: {}", line);
        }
    }).detach();
}

u64 ParseButtons(const std::string& btns) {
    const std::pair<const char*, u64> names[] = {
        {"A", hle::hid::A}, {"B", hle::hid::B}, {"X", hle::hid::X}, {"Y", hle::hid::Y}, {"L", hle::hid::L},
        {"R", hle::hid::R}, {"ZL", hle::hid::ZL}, {"ZR", hle::hid::ZR}, {"PLUS", hle::hid::Plus},
        {"MINUS", hle::hid::Minus}, {"UP", hle::hid::Up}, {"DOWN", hle::hid::Down}, {"LEFT", hle::hid::Left},
        {"RIGHT", hle::hid::Right}};
    u64 out = 0;
    size_t pos = 0;
    while (pos <= btns.size()) {
        const size_t e = std::min(btns.find('+', pos), btns.size());
        const std::string b = btns.substr(pos, e - pos);
        for (auto& [n, v] : names)
            if (b == n)
                out |= v;
        pos = e + 1;
    }
    return out;
}

// --diagnostics <run folder>: the launcher's scripted test run (setup/launcher.cpp). The game plays with the
// player's own settings but from <run folder>\save, a new empty folder, so a run (NEW GAME autosaves) can never
// write the player's save: a folder that is, holds or lies inside the configured save folder is refused, and so is
// one that already has files. mua3.ini keeps the player's save_dir (Config::Save writes user_save_dir).
std::wstring LowerPath(const fs::path& p) {
    std::error_code ec;
    fs::path full = fs::weakly_canonical(fs::absolute(p, ec), ec);
    if (ec)
        full = fs::absolute(p, ec).lexically_normal();
    std::wstring s = full.wstring();
    std::replace(s.begin(), s.end(), L'/', L'\\');
    if (s.rfind(L"\\\\?\\", 0) == 0)
        s.erase(0, 4);
    CharLowerBuffW(s.data(), DWORD(s.size()));
    while (s.size() > 3 && s.back() == L'\\')
        s.pop_back();
    return s;
}

// a == b, or a inside b
bool SameOrInside(const std::wstring& a, const std::wstring& b) {
    return a == b || (a.size() > b.size() && a.compare(0, b.size(), b) == 0 && (a[b.size()] == L'\\' || b.back() == L'\\'));
}

bool DiagnosticsSave(core::Config& cfg, const fs::path& run_dir, std::string& why) {
    const fs::path save = run_dir / "save";
    const std::wstring mine = LowerPath(save), real = LowerPath(cfg.save_dir), run = LowerPath(run_dir);
    if (SameOrInside(mine, real) || SameOrInside(real, mine) || SameOrInside(real, run)) {
        why = "the run folder " + run_dir.string() + " overlaps the save folder " + cfg.save_dir.string();
        return false;
    }
    std::error_code ec;
    if (fs::exists(save, ec) && !fs::is_empty(save, ec)) {
        why = save.string() + " already has files (a diagnostics run starts from an empty save)";
        return false;
    }
    fs::create_directories(save, ec);
    if (ec) {
        why = "cannot create " + save.string() + ": " + ec.message();
        return false;
    }
    cfg.user_save_dir = cfg.save_dir;
    cfg.save_dir = save;
    LOG_INFO("Main", "diagnostics: run folder {}, save {} (the player's {} is not used)", run_dir.string(), save.string(),
             cfg.user_save_dir.string());
    return true;
}

// Replays timed input lines (`#` starts a comment):
//   <seconds> <BUTTON>[+BUTTON...] [hold_ms]    buttons: A B X Y L R ZL ZR PLUS MINUS UP DOWN LEFT RIGHT
//   <seconds> STICK|RSTICK <x> <y> [hold_ms]    tilt the left (right) stick, -32767..32767, +y = up
//   <seconds> QUIT                              end the run (as Quit Game in the game menu)
// The times count from the start of the run; a step that is late (an earlier hold ran over) starts at once.
void RunInputScript(const fs::path& path, bool diagnostics) {
    std::ifstream f(path);
    struct Step {
        double t = 0;
        u64 buttons = 0;
        int hold = 150;
        bool stick = false, right = false, quit = false;
        int x = 0, y = 0;
    };
    std::vector<Step> steps;
    std::string line;
    while (std::getline(f, line)) {
        if (const size_t h = line.find('#'); h != std::string::npos)
            line.resize(h);
        std::istringstream ss(line);
        Step st;
        std::string what;
        if (!(ss >> st.t >> what))
            continue;
        if (what == "STICK" || what == "RSTICK") {
            st.stick = true;
            st.right = what == "RSTICK";
            st.hold = 1000;
            ss >> st.x >> st.y >> st.hold;
            st.x = std::clamp(st.x, -32767, 32767), st.y = std::clamp(st.y, -32767, 32767);
        } else if (what == "QUIT") {
            st.quit = true;
        } else {
            st.buttons = ParseButtons(what);
            ss >> st.hold;
        }
        if (st.hold <= 0)
            st.hold = 150;
        steps.push_back(st);
    }
    LOG_INFO("Main", "input script: {} steps from {}", steps.size(), path.string());
    if (diagnostics)
        LOG_INFO("Main", "diagnostics: begin");
    const auto t0 = std::chrono::steady_clock::now();
    hle::hid::SetPad(0, hle::hid::PadInput{true});
    for (auto& st : steps) {
        std::this_thread::sleep_until(t0 + std::chrono::milliseconds(s64(st.t * 1000)));
        if (st.quit) {
            LOG_INFO("Main", "input script: t={} quit", st.t);
            if (diagnostics)
                LOG_INFO("Main", "diagnostics: end");
            hle::hid::SetPad(0, hle::hid::PadInput{true});
            std::this_thread::sleep_for(std::chrono::milliseconds(300)); // (the log writer flushes these lines first)
            core::Sys().RequestExit();
            return;
        }
        hle::hid::PadInput in{true, st.buttons};
        if (st.stick)
            (st.right ? in.rx : in.lx) = st.x, (st.right ? in.ry : in.ly) = st.y;
        LOG_DEBUG("Main", "input script: t={} buttons {:#x} stick {} {}", st.t, st.buttons, st.x, st.y);
        hle::hid::SetPad(0, in);
        std::this_thread::sleep_for(std::chrono::milliseconds(st.hold));
        hle::hid::SetPad(0, hle::hid::PadInput{true});
    }
}

} // namespace

void StartAllocTrace(); // alloc_trace.cpp

// Unhandled host crash: log the exception and a symbolized stack (RelWithDebInfo has PDBs) before dying.
// The calling thread's host stack, symbolized from mua3.pdb next to the exe.
static std::string HostStack() {
    void* frames[48];
    const USHORT n = RtlCaptureStackBackTrace(0, 48, frames, nullptr);
    const HANDLE proc = GetCurrentProcess();
    static bool sym_ready = false;
    if (!sym_ready)
        sym_ready = SymInitialize(proc, nullptr, TRUE) != 0;
    std::string stack;
    for (USHORT i = 0; i < n; i++) {
        alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 256]{};
        auto* sym = reinterpret_cast<SYMBOL_INFO*>(buf);
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 255;
        DWORD64 disp = 0;
        const DWORD64 pc = reinterpret_cast<DWORD64>(frames[i]);
        stack += fmt::format("\n  #{} {}", i, sym_ready && SymFromAddr(proc, pc, &disp, sym) ? fmt::format("{}+{:#x}", sym->Name, disp)
                                                                                                : fmt::format("{:#x}", pc));
    }
    return stack;
}

// After a crash: the setup (or, in older installs, the launcher) opens its "Report a problem" form (not in automated
// test runs).
static void OpenProblemReport(const fs::path& summary) {
    if (std::getenv("MUA3_HEADLESS"))
        return;
    wchar_t exe[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    fs::path launcher = fs::path(exe).parent_path() / "MUA3Setup.exe";
    if (!fs::exists(launcher))
        launcher = fs::path(exe).parent_path() / "MUA3Launcher.exe";
    if (!fs::exists(launcher))
        return;
    std::error_code ec;
    std::wstring cmd = L"\"" + launcher.wstring() + L"\" --crash-report \"" + fs::absolute(summary, ec).wstring() + L"\"";
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, DETACHED_PROCESS, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
}

static LONG WINAPI CrashFilter(EXCEPTION_POINTERS* ep) {
    static std::atomic<bool> once{false};
    if (once.exchange(true))
        return EXCEPTION_CONTINUE_SEARCH;
    const auto* er = ep->ExceptionRecord;
    LOG_ERROR("Crash", "unhandled exception {:#x} at {:p} (info {:#x} {:#x})", u32(er->ExceptionCode), er->ExceptionAddress,
              er->NumberParameters > 0 ? er->ExceptionInformation[0] : 0, er->NumberParameters > 1 ? er->ExceptionInformation[1] : 0);
    HANDLE proc = GetCurrentProcess();
    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    SymInitialize(proc, nullptr, TRUE);
    CONTEXT ctx = *ep->ContextRecord;
    STACKFRAME64 f{};
    f.AddrPC = {ctx.Rip, 0, AddrModeFlat};
    f.AddrFrame = {ctx.Rbp, 0, AddrModeFlat};
    f.AddrStack = {ctx.Rsp, 0, AddrModeFlat};
    for (int i = 0; i < 32; i++) {
        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, proc, GetCurrentThread(), &f, &ctx, nullptr, SymFunctionTableAccess64,
                         SymGetModuleBase64, nullptr) || !f.AddrPC.Offset)
            break;
        alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 256]{};
        auto* sym = reinterpret_cast<SYMBOL_INFO*>(buf);
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 255;
        DWORD64 disp = 0;
        IMAGEHLP_LINE64 line{sizeof(line)};
        DWORD ldisp = 0;
        const bool has_sym = SymFromAddr(proc, f.AddrPC.Offset, &disp, sym);
        const bool has_line = SymGetLineFromAddr64(proc, f.AddrPC.Offset, &ldisp, &line);
        LOG_ERROR("Crash", "  #{} {:#x} {}+{:#x} {}:{}", i, f.AddrPC.Offset, has_sym ? sym->Name : "?", disp,
                  has_line ? line.FileName : "", has_line ? line.LineNumber : 0);
    }
    OpenProblemReport(common::WriteCrashReport("mua3", ep, "unhandled exception"));
    std::fflush(nullptr);
    return EXCEPTION_CONTINUE_SEARCH;
}

// MUA3_SAMPLE_GPU=1: statistical profiler for the GPU command thread. Samples its instruction pointer at ~2 kHz
// and logs the hottest functions (leaf, and the first frame inside the renderer) every 10 seconds.
static void StartGpuSampler() {
    std::thread([] {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        HANDLE th = static_cast<HANDLE>(gpu::G().NativeThread());
        HANDLE proc = GetCurrentProcess();
        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
        SymInitialize(proc, nullptr, TRUE);
        auto name_of = [&](DWORD64 pc) -> std::string {
            alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 256]{};
            auto* sym = reinterpret_cast<SYMBOL_INFO*>(buf);
            sym->SizeOfStruct = sizeof(SYMBOL_INFO);
            sym->MaxNameLen = 255;
            DWORD64 disp = 0;
            return SymFromAddr(proc, pc, &disp, sym) ? std::string(sym->Name) : fmt::format("{:#x}", pc);
        };
        std::map<std::string, u32> leaf, owner;
        u32 total = 0;
        auto t0 = std::chrono::steady_clock::now();
        for (;;) {
            std::this_thread::sleep_for(std::chrono::microseconds(500));
            if (SuspendThread(th) == DWORD(-1))
                continue;
            // Only capture raw return addresses while the thread is suspended: it may hold the heap or dbghelp
            // locks, so anything that allocates must wait until it runs again.
            DWORD64 pcs[12];
            int npc = 0;
            CONTEXT ctx{};
            ctx.ContextFlags = CONTEXT_FULL;
            if (GetThreadContext(th, &ctx)) {
                for (; npc < 12 && ctx.Rip; npc++) {
                    pcs[npc] = ctx.Rip;
                    DWORD64 image = 0;
                    RUNTIME_FUNCTION* fn = RtlLookupFunctionEntry(ctx.Rip, &image, nullptr);
                    if (!fn) { // leaf function: return address on top of the stack
                        ctx.Rip = *reinterpret_cast<DWORD64*>(ctx.Rsp);
                        ctx.Rsp += 8;
                        continue;
                    }
                    void* handler_data = nullptr;
                    DWORD64 frame = 0;
                    RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, ctx.Rip, fn, &ctx, &handler_data, &frame, nullptr);
                }
            }
            ResumeThread(th);
            if (npc) {
                std::string first, mine;
                for (int i = 0; i < npc; i++) {
                    std::string n = name_of(pcs[i]);
                    if (first.empty())
                        first = n;
                    if (n.rfind("gpu::", 0) == 0 || n.rfind("core::", 0) == 0) {
                        mine = n;
                        break;
                    }
                }
                leaf[first]++;
                owner[mine.empty() ? first : mine]++;
                total++;
            }
            if (std::chrono::steady_clock::now() - t0 > std::chrono::seconds(10) && total) {
                t0 = std::chrono::steady_clock::now();
                for (auto* m : {&leaf, &owner}) {
                    std::vector<std::pair<u32, std::string>> v;
                    for (auto& [k, c] : *m)
                        v.push_back({c, k});
                    std::sort(v.rbegin(), v.rend());
                    std::string line;
                    for (size_t i = 0; i < v.size() && i < 25; i++)
                        line += fmt::format("\n    {:5.1f}% {}", 100.0 * v[i].first / total, v[i].second);
                    LOG_INFO("Sampler", "GPU thread, {} samples, by {}:{}", total, m == &leaf ? "leaf" : "renderer function", line);
                    m->clear();
                }
                total = 0;
            }
        }
    }).detach();
}

int main(int argc, char** argv) {
    SetUnhandledExceptionFilter(CrashFilter);
    std::signal(SIGABRT, [](int) { // fatal stops (Panic, std::terminate) end here
        LOG_ERROR("Crash", "abort(){}", HostStack());
        OpenProblemReport(common::WriteCrashReport("mua3", nullptr, "the game stopped (fatal error, see mua3.log)"));
        std::fflush(nullptr);
    });
    // C++ throws, logged where they happen (the first few): one escaping a thread kills the process with little
    // trace otherwise.
    AddVectoredExceptionHandler(0, [](EXCEPTION_POINTERS* ep) -> LONG {
        if (ep->ExceptionRecord->ExceptionCode == 0xE06D7363) {
            static std::atomic<int> logged{0};
            if (logged++ < 6)
                LOG_WARN("Crash", "C++ exception thrown{}", HostStack());
        }
        return EXCEPTION_CONTINUE_SEARCH;
    });
    std::set_terminate([] {
        LOG_ERROR("Crash", "std::terminate (uncaught C++ exception)");
        std::fflush(nullptr);
        std::abort();
    });
    // Stack overflows need stack left for the filter to run.
    ULONG guarantee = 64 * 1024;
    SetThreadStackGuarantee(&guarantee);
    // Child-process mode used by the shader cache for compiles that may not finish (see ShaderCache).
    if (argc == 5 && std::string(argv[1]) == "--compile-shader")
        return gpu::CompileShaderFile(argv[2], argv[3], argv[4]) ? 0 : 1;
    if (argc == 2 && std::string(argv[1]) == "--compile-server")
        return gpu::CompileServer();
    // The launcher's last build step: every shader translated and compiled before the first launch.
    if (argc == 3 && std::string(argv[1]) == "--build-shaders")
        return gpu::BuildShadersAhead(fs::u8path(argv[2]));
    // Developer check: the launch prewarm of ./shader_dump, timed (`--bench-shaders [workers, 0 = in-game]`).
    if ((argc == 2 || argc == 3) && std::string(argv[1]) == "--bench-shaders") {
        common::SetLogFile("bench.log");
        return gpu::BenchShaders(argc == 3 ? unsigned(std::atoi(argv[2])) : 0u);
    }
    // Developer check of the motion-vector shader variants: `--test-motion <shader_dump dir> [programs, 0 = all]`.
    if ((argc == 3 || argc == 4) && std::string(argv[1]) == "--test-motion")
        return gpu::TestMotionVectors(fs::u8path(argv[2]), argc == 4 ? std::atoi(argv[3]) : 0);
    core::Config cfg;
    cfg.Load("mua3.ini");
    int watch_seconds = 0;
    bool headless = false;
    int autopress_after = 0; // test aid: tap A every 3 s after N seconds (headless)
    fs::path diag_dir;       // --diagnostics <run folder>: the launcher's scripted test run (DiagnosticsSave)
    fs::path input_script;   // test aid: timed button presses ("<seconds> <BUTTON>[+BUTTON] [hold ms]" per line)
    int debug_chapter = -1;  // debug: --chapter N / --stage ST0N0 (app/debug_chapter.cpp)
    fs::path save_template;  // debug: seeds an empty debug_save
    std::string bad_stage, bad_checkpoint;
    int debug_section = 0, debug_point = 0; // debug: --checkpoint S:P (DataSavePoint.bin section and point)
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--trace")
            cfg.log_level = 0;
        else if (a == "--debug")
            cfg.log_level = 1;
        else if (a == "--game" && i + 1 < argc)
            cfg.game_dir = fs::u8path(argv[++i]);
        else if (a == "--watch" && i + 1 < argc)
            watch_seconds = std::atoi(argv[++i]);
        else if (a == "--headless")
            headless = true;
        else if (a == "--autopress" && i + 1 < argc)
            autopress_after = std::atoi(argv[++i]);
        else if (a == "--input-script" && i + 1 < argc)
            input_script = fs::u8path(argv[++i]);
        else if (a == "--diagnostics" && i + 1 < argc)
            diag_dir = fs::u8path(argv[++i]);
        else if (a == "--chapter" && i + 1 < argc)
            debug_chapter = std::atoi(argv[++i]);
        else if (a == "--stage" && i + 1 < argc) {
            debug_chapter = app::debug_chapter::ChapterOfStage(argv[++i]);
            bad_stage = debug_chapter < 0 ? argv[i] : "";
        }
        else if (a == "--checkpoint" && i + 1 < argc) {
            if (!app::debug_chapter::ParseCheckpoint(argv[++i], debug_section, debug_point))
                bad_checkpoint = argv[i];
        }
        else if (a == "--save-template" && i + 1 < argc)
            save_template = fs::u8path(argv[++i]);
    }
    common::SetLogLevel(common::LogLevel(std::clamp(cfg.log_level, 0, 5)));
    common::RotateLog("mua3", 5); // the previous run's log goes to logs/
    common::SetLogFile("mua3.log");
    LOG_INFO("Main", "MUA3 Native {}", common::Version());
    if (!bad_checkpoint.empty())
        LOG_WARN("Debug", "--checkpoint {}: takes <section>:<point> (DataSavePoint.bin numbering), e.g. 1:0",
                 bad_checkpoint);
    if (!diag_dir.empty()) {
        if (debug_chapter >= 0) {
            LOG_ERROR("Main", "--diagnostics cannot be combined with --chapter / --stage");
            return 5;
        }
        std::string why;
        if (!DiagnosticsSave(cfg, diag_dir, why)) {
            LOG_ERROR("Main", "diagnostics refused: {}", why);
            return 5;
        }
        if (input_script.empty())
            input_script = diag_dir / "script.txt";
    }
    if (debug_chapter >= 0)
        app::debug_chapter::Prepare(cfg, debug_chapter, debug_section, debug_point, save_template);
    else if (!bad_stage.empty())
        LOG_WARN("Debug", "--stage {}: takes ST010 .. ST120 (the first stage of story chapters 1-12); use --chapter <N> for others", bad_stage);
    timeBeginPeriod(1);
    StartAllocTrace(); // MUA3_TRACE_VALLOC (diagnostic)
    // The exe names its PDB without a path (/PDBALTPATH): let in-process symbol lookups (profilers, stack dumps)
    // search the exe's folder.
    {
        wchar_t exe[MAX_PATH]{};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        SetEnvironmentVariableW(L"_NT_SYMBOL_PATH", fs::path(exe).parent_path().c_str());
    }

    core::System sys(cfg);
    std::string error;
    if (!sys.Load(error)) {
        LOG_ERROR("Main", "Load failed: {}", error);
        MessageBoxA(nullptr, error.c_str(), "MUA3 Native", MB_ICONERROR);
        return 1;
    }

    gpu::G().on_present = [](const gpu::PresentInfo& p) {
        g_stats.presents++;
        g_stats.width = p.width;
        g_stats.height = p.height;
    };

    if (headless) {
        sys.Start();
        for (int t = 1;; t++) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            if (watch_seconds > 0 && t % watch_seconds == 0)
                sys.Kern().DumpAllThreads();
            if (autopress_after > 0 && t >= autopress_after && t % 3 == 0) {
                LOG_INFO("Main", "autopress: A");
                hle::hid::PadInput p{true, hle::hid::A};
                hle::hid::SetPad(0, p);
                std::this_thread::sleep_for(std::chrono::milliseconds(150));
                hle::hid::SetPad(0, hle::hid::PadInput{true});
            }
        }
    }

    platform::Display display;
    const bool display_ok = display.Init(cfg, "Marvel Ultimate Alliance 3: The Black Order");
    if (display_ok)
        display.SetIcon(cfg.game_dir / "control" / "icon_AmericanEnglish.dat");
    if (!display_ok) {
        MessageBoxA(nullptr, "Failed to create the D3D11 window.", "MUA3 Native", MB_ICONERROR);
        return 1;
    }
    // High-DPI monitors: rasterise the overlay font and size its widgets at the display's scale (sharp, readable
    // text on 4K instead of tiny 13 px text).
    g_ui = std::clamp(SDL_GetDisplayContentScale(SDL_GetDisplayForWindow(display.Window())), 1.0f, 3.0f);
    if (g_ui > 1.0f) {
        ImGui::GetStyle().ScaleAllSizes(g_ui);
        ImGuiIO& io = ImGui::GetIO();
        io.Fonts->Clear();
        ImFontConfig fc;
        fc.SizePixels = 13.0f * g_ui;
        io.Fonts->AddFontDefault(&fc);
    }
    platform::Input input;
    input.Init(cfg.input, cfg.camera_speed, cfg.button_layout);
    input.SetMouseCamera(cfg.mouse_camera, cfg.mouse_sensitivity);
    input.SetDeadzones(cfg.left_deadzone, cfg.right_deadzone);
    platform::Audio().Init(cfg.audio, cfg.mute ? 0 : cfg.volume);
    hle::SetAudioOutput([](const s16* pcm, size_t frames) { platform::Audio().Push(pcm, frames); },
                        [] { return platform::Audio().Queued(); });

    gpu::D3D11Renderer renderer(display.Device(), display.Context());
    // resolution_scale 0 = auto: render the game's screen-sized targets (1080p docked, 720p handheld) at the
    // smallest multiple of 1/3 that covers the output: 1440p renders natively at 4/3 (not 2x then scaled down),
    // 4K at 2x, so the frame is never an upscaled 1080p one either.
    u32 scale3 = u32(std::clamp(cfg.resolution_scale, 1, 4)) * 3;
    if (cfg.resolution_scale <= 0) {
        const u32 base = cfg.docked ? 1080 : 720;
        scale3 = std::clamp<u32>((display.PixelHeight() * 3 + base - 1) / base, 3, 12);
    }
    LOG_INFO("Display", "output {} px high, internal render scale {:.3g}x{}", display.PixelHeight(), scale3 / 3.0,
             cfg.resolution_scale <= 0 ? " (auto)" : "");
    renderer.SetOutputHeight(display.PixelHeight()); // (the upscaler's target, upscaler=1)
    renderer.SetResolutionScale3(scale3);
    hle::menu::graphics::SetRenderScale(scale3); // the GRAPHICS page shows the render size
    gpu::G().SetRenderer(&renderer);
    gpu::InstallNvnEncoder(gpu::G());
    g_renderer = &renderer;
    g_display = &display;
    // First launch: harvest every shader program the game ships into the prewarm manifest (about 30 s, in the
    // background); the renderer picks them up and compiles them while the intro and menus run.
    if (const fs::path shaders = cfg.game_dir / "romfs" / "SHADER", marker = "shader_dump/harvested.txt";
        fs::exists(shaders) && !fs::exists(marker)) {
        std::thread([shaders, marker, &renderer] {
            const size_t n = gpu::shader::HarvestPrograms(shaders, "shader_dump");
            std::ofstream(marker) << n << "\n";
            LOG_INFO("Shader", "harvested {} shader programs from the game data", n);
            renderer.Shaders()->RequestRescan();
        }).detach();
    }
    if (std::getenv("MUA3_SAMPLE_GPU"))
        StartGpuSampler();
    // Test aid: MUA3_DUMP_RT=<seconds> dumps all colour render targets once.
    if (const char* t = std::getenv("MUA3_DUMP_RT"))
        std::thread([s = std::atoi(t)] {
            std::this_thread::sleep_for(std::chrono::seconds(s));
            g_renderer->dump_surfaces_ = true;
        }).detach();
    display.context_mutex = &gpu::ContextMutex();
    display.context_epoch = &gpu::ContextEpoch();
    display.frame_source = [&](u32& w, u32& h) { return renderer.LatestFrame(w, h); };
    // Device lost: the renderer drops its objects before the device is recreated and continues on the new one.
    display.device_lost_check = [&] { return renderer.DeviceRemoved(); };
    display.release_device = [&] { renderer.ReleaseDevice(); };
    display.restore_device = [&](ID3D11Device* d, ID3D11DeviceContext* c) {
        renderer.RestoreDevice(d, c);
        g_device_toast_until = std::chrono::steady_clock::now() + std::chrono::seconds(4);
    };

    bool show_overlay = cfg.show_debug_overlay;
    display.draw_overlay = [&] {
        DrawOverlay(cfg, input, show_overlay);
        platform::bugcap::DrawToast();
    };
    platform::bugcap::Init(cfg.bug_capture, cfg.save_dir);

    sys.Start();
    // Test aid: MUA3_PROFILE=<delay seconds> samples the GPU thread for 10 s.
    if (const char* p = std::getenv("MUA3_PROFILE"))
        common::StartSamplingProfiler(gpu::G().NativeThreadHandle(), "GPU thread", 10, std::atoi(p));
    auto next_dump = std::chrono::steady_clock::now() + std::chrono::seconds(watch_seconds);
    if (!input_script.empty()) {
        autopress_after = -1; // player 1 is driven by the script
        std::thread([input_script, diagnostics = !diag_dir.empty()] { RunInputScript(input_script, diagnostics); }).detach();
    }
    if (autopress_after > 0) // test aid: tap A every 3 s after N seconds
        std::thread([autopress_after] {
            std::this_thread::sleep_for(std::chrono::seconds(autopress_after));
            for (int n = 0;; n++) {
                // Cycle + (title screen), Y (close patch notes) and A (menus/dialogs).
                const u64 b = (n % 3 == 0) ? hle::hid::Plus : (n % 3 == 1) ? hle::hid::Y : hle::hid::A;
                LOG_INFO("Main", "autopress: {}", b == hle::hid::Plus ? "+" : b == hle::hid::Y ? "Y" : "A");
                hle::hid::SetPad(0, hle::hid::PadInput{true, b});
                std::this_thread::sleep_for(std::chrono::milliseconds(150));
                hle::hid::SetPad(0, hle::hid::PadInput{true});
                std::this_thread::sleep_for(std::chrono::seconds(3));
            }
        }).detach();
    // MUA3_HEADLESS=1 (automated test runs): hidden window, physical controllers ignored (only mua3_cmd.txt drives).
    const bool test_instance = std::getenv("MUA3_HEADLESS") != nullptr;
    u32 settings_generation = sys.ConfigGeneration();
    while (display.PumpEvents() && !sys.ExitRequested()) {
        if (const u32 g = sys.ConfigGeneration(); g != settings_generation) {
            settings_generation = g;
            ApplyGameSettings(sys.CfgSnapshot(), cfg, display, input, test_instance);
        }
        if (autopress_after == 0 && !g_script_active && !test_instance) // the test aids drive player 1 themselves
            input.Update(display.Focused());
        input.MouseCamera(display.Window(), display.Focused());
        PollCommandFile(&input);
        platform::bugcap::Poll(renderer, display);
        display.BeginFrame();
        display.EndFrame();
        if (display.DeviceLost()) // (recreates the device and swap chain; retried next frame if it fails)
            display.RecoverDevice();
        // Present when the game has a new frame (in step with its frame rate, no beat against a separate UI
        // clock); the timeout keeps the UI responsive while the game shows nothing new (loading, paused).
        const bool new_frame = WaitForSingleObject(static_cast<HANDLE>(renderer.NewFrameEvent()), 50) == WAIT_OBJECT_0;
        { // frame pacing stats (display side)
            static auto last = std::chrono::steady_clock::now(), t0 = last;
            static double worst = 0, sum = 0;
            static int n = 0, late = 0;
            const auto now = std::chrono::steady_clock::now();
            if (new_frame) {
                const double ms = std::chrono::duration<double, std::milli>(now - last).count();
                last = now;
                worst = std::max(worst, ms);
                sum += ms;
                n++;
                late += ms > 20.0;
            }
            if (now - t0 > std::chrono::seconds(5)) {
                if (n)
                    LOG_INFO("Main", "display: {} frames, avg {:.2f} ms, worst {:.1f} ms, {} over 20 ms", n, sum / n, worst, late);
                // Memory: whole-process stalls of hundreds of ms (every thread, GPU idle) on players' PCs look like
                // paging; this line tells (process memory, page faults per second, the system's free RAM and commit).
                {
                    static DWORD last_faults = 0;
                    PROCESS_MEMORY_COUNTERS_EX pm{};
                    pm.cb = sizeof(pm);
                    MEMORYSTATUSEX ms{};
                    ms.dwLength = sizeof(ms);
                    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pm), sizeof(pm)) &&
                        GlobalMemoryStatusEx(&ms)) {
                        const double secs = std::chrono::duration<double>(now - t0).count();
                        const DWORD faults = pm.PageFaultCount - last_faults;
                        // texture uploads per second (#8: CPU-rewritten glyph caches uploaded again on AMD)
                        auto& ts = gpu::g_texture_stats;
                        const u64 tex[7] = {ts.reuploads.load(), ts.reupload_bytes.load(), ts.partial.load(),
                                            ts.unchanged.load(), ts.uploads.load(), ts.upload_bytes.load(),
                                            ts.staged.load()};
                        static u64 last_tex[7]{};
                        auto rate = [&](int i) { return double(tex[i] - last_tex[i]) / std::max(secs, 0.001); };
                        if (last_faults)
                            LOG_INFO("Main", "memory: process {} MB private, {} MB in RAM, {:.0f} page faults/s; system RAM "
                                             "{} of {} MB free ({}% used), commit {} of {} MB free | textures: {:.1f} "
                                             "re-uploads/s ({:.1f} MB/s, {:.1f}/s partial, {:.1f}/s staged), {:.1f} unchanged rewrites/s, "
                                             "{:.1f} new/s ({:.1f} MB/s)",
                                     pm.PrivateUsage >> 20, pm.WorkingSetSize >> 20, faults / std::max(secs, 0.001),
                                     ms.ullAvailPhys >> 20, ms.ullTotalPhys >> 20, ms.dwMemoryLoad,
                                     ms.ullAvailPageFile >> 20, ms.ullTotalPageFile >> 20, rate(0), rate(1) / (1 << 20),
                                     rate(2), rate(6), rate(3), rate(4), rate(5) / (1 << 20));
                        std::copy(std::begin(tex), std::end(tex), last_tex);
                        last_faults = pm.PageFaultCount;
                    }
                }
                t0 = now;
                worst = sum = 0;
                n = late = 0;
            }
        }
        if (watch_seconds > 0 && std::chrono::steady_clock::now() >= next_dump) {
            sys.Kern().DumpAllThreads();
            next_dump += std::chrono::seconds(watch_seconds);
        }
    }
    LOG_INFO("Main", "{}", sys.ExitRequested() ? "Quit Game chosen in the game menu, exiting" : "Window closed, exiting");
    platform::Audio().Shutdown();
    display.Shutdown();
    std::fflush(stderr);
    TerminateProcess(GetCurrentProcess(), 0);
    return 0;
}
