// MUA3 runtime entry point: window/input/audio on the main thread, the game on its own threads.
#include <algorithm>
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
#include "gpu/gpu.h"
#include "platform/platform.h"

namespace {

gpu::D3D11Renderer* g_renderer = nullptr;

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
            for (auto& t : g_renderer->RecentTextures()) {
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
    if (autopress_after > 0) // test aid: tap A every 3 s after N seconds
        std::thread([autopress_after] {
            std::this_thread::sleep_for(std::chrono::seconds(autopress_after));
            for (;;) {
                LOG_INFO("Main", "autopress: A");
                hle::hid::SetPad(0, hle::hid::PadInput{true, hle::hid::A});
                std::this_thread::sleep_for(std::chrono::milliseconds(150));
                hle::hid::SetPad(0, hle::hid::PadInput{true});
                std::this_thread::sleep_for(std::chrono::seconds(3));
            }
        }).detach();
    while (display.PumpEvents()) {
        if (autopress_after <= 0) // the test aid drives player 1 itself
            input.Update(display.Focused());
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
