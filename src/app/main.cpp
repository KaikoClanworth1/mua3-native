// MUA3 runtime entry point: window/input/audio on the main thread, the game on its own threads.
#include <algorithm>
#include <chrono>
#include <thread>

#include <imgui.h>
#include <windows.h>
#include <timeapi.h>

#include "core/config.h"
#include "core/kernel/kernel.h"
#include "core/system.h"
#include "core/hle/hid.h"
#include "gpu/gpu.h"
#include "platform/platform.h"

namespace {

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
    ImGui::Text("GPU submits: %llu   methods: %llu", (unsigned long long)g.submits, (unsigned long long)g.methods);
    ImGui::Text("Input: %s   Audio: %s", input.DeviceName().c_str(), platform::Audio().BackendName().c_str());
    ImGui::Separator();
    ImGui::TextWrapped("CPU: dynarmic JIT (interim)   GPU: command stream accepted, D3D11 renderer pending");
    if (ImGui::Button("Dump guest threads to log"))
        core::Sys().Kern().DumpAllThreads();
    ImGui::End();
}

} // namespace

int main(int argc, char** argv) {
    core::Config cfg;
    cfg.Load("mua3.ini");
    int watch_seconds = 0;
    bool headless = false;
    int autopress_after = 0; // test aid: tap A every 3 s after N seconds (headless)
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

    gpu::G().on_present = [](u64, u32 w, u32 h, u32, u32) {
        g_stats.presents++;
        g_stats.width = w;
        g_stats.height = h;
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

    bool show_overlay = cfg.show_debug_overlay;
    display.draw_overlay = [&] { DrawOverlay(cfg, input, show_overlay); };

    sys.Start();
    auto next_dump = std::chrono::steady_clock::now() + std::chrono::seconds(watch_seconds);
    while (display.PumpEvents()) {
        input.Update(display.Focused());
        display.BeginFrame();
        display.EndFrame();
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
