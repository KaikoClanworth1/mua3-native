// MUA3 runtime entry point: window/input/audio on the main thread, the game on its own threads.
#include <algorithm>
#include <fstream>
#include <sstream>
#include <chrono>
#include <thread>

#include <imgui.h>
#include <windows.h>
#include <timeapi.h>

#include "common/profiler.h"
#include "core/config.h"
#include "core/kernel/kernel.h"
#include "core/system.h"
#include "core/hle/hid.h"
#include "gpu/d3d11/renderer_d3d11.h"
#include "gpu/d3d11/shader_cache.h"
#include "gpu/gpu.h"
#include "platform/platform.h"

namespace {

gpu::D3D11Renderer* g_renderer = nullptr;
std::atomic<bool> g_script_active{false};

struct Stats {
    std::atomic<u64> presents{0};
    std::atomic<u32> width{0}, height{0};
    double fps = 0;
    u64 last_presents = 0;
    std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
} g_stats;

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
    if (cfg.show_fps) {
        ImGui::SetNextWindowPos(ImVec2(8, 8));
        ImGui::SetNextWindowBgAlpha(0.35f);
        ImGui::Begin("##fps", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                         ImGuiWindowFlags_NoNav);
        ImGui::Text("%.1f FPS", g_stats.fps);
        ImGui::End();
    }
    if (!show)
        return;
    ImGui::SetNextWindowPos(ImVec2(8, 48), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(460, 0), ImGuiCond_FirstUseEver);
    ImGui::Begin("MUA3 Native (F1)");
    auto& g = gpu::G();
    ImGui::Text("Game frames presented: %llu (%.1f fps)", (unsigned long long)g_stats.presents.load(), g_stats.fps);
    ImGui::Text("Swapchain: %ux%u", g_stats.width.load(), g_stats.height.load());
    ImGui::Text("GPU submits: %llu   methods: %llu   draws: %llu", (unsigned long long)g.submits.load(),
                (unsigned long long)g.methods.load(), (unsigned long long)g.draws.load());
    ImGui::Text("Input: %s   Audio: %s", input.DeviceName().c_str(), platform::Audio().BackendName().c_str());
    ImGui::Separator();
    ImGui::TextWrapped("CPU: dynarmic JIT (interim)   GPU: command stream accepted, D3D11 renderer pending");
    if (ImGui::Button("Dump guest threads to log"))
        core::Sys().Kern().DumpAllThreads();
    ImGui::SameLine();
    if (ImGui::Button("Dump render targets") && g_renderer)
        g_renderer->dump_surfaces_ = true;
    ImGui::End();

    if (g_renderer) {
        static bool show_tex = std::getenv("MUA3_TEXVIEW") != nullptr;
        if (show_tex) {
            ImGui::SetNextWindowPos(ImVec2(500, 48), ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowSize(ImVec2(760, 640), ImGuiCond_FirstUseEver);
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
    std::thread([lines] {
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
            } else if (cmd == "shot" && g_renderer) {
                g_renderer->RequestFrameShot(fs::u8path(arg));
            } else if (cmd == "dump") {
                core::Sys().Kern().DumpAllThreads(arg == "host");
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

// Replays "<seconds> <BUTTON>[+BUTTON...] [hold_ms]" lines; buttons: A B X Y L R ZL ZR PLUS MINUS UP DOWN LEFT RIGHT.
void RunInputScript(const fs::path& path) {
    std::ifstream f(path);
    struct Step {
        double t;
        u64 buttons;
        int hold;
    };
    std::vector<Step> steps;
    std::string line;
    const std::pair<const char*, u64> names[] = {
        {"A", hle::hid::A}, {"B", hle::hid::B}, {"X", hle::hid::X}, {"Y", hle::hid::Y}, {"L", hle::hid::L},
        {"R", hle::hid::R}, {"ZL", hle::hid::ZL}, {"ZR", hle::hid::ZR}, {"PLUS", hle::hid::Plus},
        {"MINUS", hle::hid::Minus}, {"UP", hle::hid::Up}, {"DOWN", hle::hid::Down}, {"LEFT", hle::hid::Left},
        {"RIGHT", hle::hid::Right}};
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#')
            continue;
        std::istringstream ss(line);
        Step st{0, 0, 150};
        std::string btns;
        ss >> st.t >> btns >> st.hold;
        if (!st.hold)
            st.hold = 150;
        size_t pos = 0;
        while (pos <= btns.size()) {
            const size_t e = std::min(btns.find('+', pos), btns.size());
            const std::string b = btns.substr(pos, e - pos);
            for (auto& [n, v] : names)
                if (b == n)
                    st.buttons |= v;
            pos = e + 1;
        }
        steps.push_back(st);
    }
    const auto t0 = std::chrono::steady_clock::now();
    hle::hid::SetPad(0, hle::hid::PadInput{true});
    for (auto& st : steps) {
        std::this_thread::sleep_until(t0 + std::chrono::milliseconds(s64(st.t * 1000)));
        LOG_INFO("Main", "input script: t={} buttons {:#x}", st.t, st.buttons);
        hle::hid::SetPad(0, hle::hid::PadInput{true, st.buttons});
        std::this_thread::sleep_for(std::chrono::milliseconds(st.hold));
        hle::hid::SetPad(0, hle::hid::PadInput{true});
    }
}

} // namespace

int main(int argc, char** argv) {
    // Child-process mode used by the shader cache for compiles that may not finish (see ShaderCache).
    if (argc == 5 && std::string(argv[1]) == "--compile-shader")
        return gpu::CompileShaderFile(argv[2], argv[3], argv[4]) ? 0 : 1;
    core::Config cfg;
    cfg.Load("mua3.ini");
    int watch_seconds = 0;
    bool headless = false;
    int autopress_after = 0; // test aid: tap A every 3 s after N seconds (headless)
    fs::path input_script;   // test aid: timed button presses ("<seconds> <BUTTON>[+BUTTON] [hold ms]" per line)
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
    }
    common::SetLogLevel(common::LogLevel(std::clamp(cfg.log_level, 0, 5)));
    common::SetLogFile("mua3.log");
    timeBeginPeriod(1);

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
    if (!display.Init(cfg, "Marvel Ultimate Alliance 3: The Black Order")) {
        MessageBoxA(nullptr, "Failed to create the D3D11 window.", "MUA3 Native", MB_ICONERROR);
        return 1;
    }
    platform::Input input;
    input.Init(cfg.input);
    platform::Audio().Init(cfg.audio, cfg.volume);

    gpu::D3D11Renderer renderer(display.Device(), display.Context());
    gpu::G().SetRenderer(&renderer);
    g_renderer = &renderer;
    // Test aid: MUA3_DUMP_RT=<seconds> dumps all colour render targets once.
    if (const char* t = std::getenv("MUA3_DUMP_RT"))
        std::thread([s = std::atoi(t)] {
            std::this_thread::sleep_for(std::chrono::seconds(s));
            g_renderer->dump_surfaces_ = true;
        }).detach();
    display.context_mutex = &gpu::ContextMutex();
    display.frame_source = [&](u32& w, u32& h) { return renderer.LatestFrame(w, h); };

    bool show_overlay = cfg.show_debug_overlay;
    display.draw_overlay = [&] { DrawOverlay(cfg, input, show_overlay); };

    sys.Start();
    // Test aid: MUA3_PROFILE=<delay seconds> samples the GPU thread for 10 s.
    if (const char* p = std::getenv("MUA3_PROFILE"))
        common::StartSamplingProfiler(gpu::G().NativeThreadHandle(), "GPU thread", 10, std::atoi(p));
    auto next_dump = std::chrono::steady_clock::now() + std::chrono::seconds(watch_seconds);
    if (!input_script.empty()) {
        autopress_after = -1; // player 1 is driven by the script
        std::thread([input_script] { RunInputScript(input_script); }).detach();
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
    while (display.PumpEvents()) {
        if (autopress_after == 0 && !g_script_active) // the test aids drive player 1 themselves
            input.Update(display.Focused());
        PollCommandFile(&input);
        display.BeginFrame();
        display.EndFrame();
        display.PaceFrame();
        if (watch_seconds > 0 && std::chrono::steady_clock::now() >= next_dump) {
            sys.Kern().DumpAllThreads();
            next_dump += std::chrono::seconds(watch_seconds);
        }
    }
    LOG_INFO("Main", "Window closed, exiting");
    platform::Audio().Shutdown();
    display.Shutdown();
    std::fflush(stderr);
    TerminateProcess(GetCurrentProcess(), 0);
    return 0;
}
