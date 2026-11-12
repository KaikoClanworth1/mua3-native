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
#include <timeapi.h>
#include <dbghelp.h>

#include "common/crash.h"
#include "common/profiler.h"
#include "core/config.h"
#include "core/kernel/kernel.h"
#include "core/system.h"
#include "core/hle/audio_output.h"
#include "core/hle/hid.h"
#include "core/hle/menu_graphics.h"
#include "gpu/d3d11/renderer_d3d11.h"
#include "platform/bug_capture.h"
#include "gpu/d3d11/shader_cache.h"
#include "core/cpu/cpu.h"
#include "gpu/shader/harvest.h"
#include "gpu/gpu.h"
#include "gpu/nvn_encoder.h"
#include "platform/platform.h"

namespace {

gpu::D3D11Renderer* g_renderer = nullptr;
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
        input.SetCameraSpeed(n.camera_speed);
    cfg.camera_speed = n.camera_speed;
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
    if (cfg.show_fps) {
        ImGui::SetNextWindowPos(UiVec(8, 8));
        ImGui::SetNextWindowBgAlpha(0.35f);
        ImGui::Begin("##fps", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                         ImGuiWindowFlags_NoNav);
        ImGui::Text("%.1f FPS", g_stats.fps);
        ImGui::End();
    }
    if (g_renderer && g_renderer->Shaders() && g_renderer->Shaders()->pending > 0) {
        // Like a PC game's first-launch shader compilation: everything the game ships is compiled up front.
        // Before the game shows its first frame this is the whole screen; afterwards a corner box.
        auto* sc = g_renderer->Shaders();
        const u32 pending = sc->pending.load(), total = std::max(sc->prewarm_total.load(), pending);
        const float frac = total ? float(total - pending) / float(total) : 0.0f;
        const ImVec2 ds = ImGui::GetIO().DisplaySize;
        const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                       ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav;
        char label[64];
        std::snprintf(label, sizeof(label), "%u / %u", total - pending, total);
        if (g_stats.presents == 0) {
            ImGui::SetNextWindowPos(ImVec2(ds.x * 0.5f, ds.y * 0.5f), 0, ImVec2(0.5f, 0.5f));
            ImGui::SetNextWindowBgAlpha(0.9f);
            ImGui::Begin("##shaders", nullptr, flags);
            ImGui::Text("Preparing shaders for your GPU");
            ImGui::ProgressBar(frac, UiVec(520, 24), label);
            ImGui::TextDisabled("Compiled once and kept; the game continues loading meanwhile.");
            ImGui::End();
        } else {
            ImGui::SetNextWindowPos(ImVec2(ds.x - 8 * g_ui, 8 * g_ui), 0, ImVec2(1, 0));
            ImGui::SetNextWindowBgAlpha(0.5f);
            ImGui::Begin("##shaders", nullptr, flags);
            ImGui::Text("Compiling shaders");
            ImGui::ProgressBar(frac, UiVec(220, 16), label);
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
    // The launcher's last build step: every shader translated and compiled before the first launch.
    if (argc == 3 && std::string(argv[1]) == "--build-shaders")
        return gpu::BuildShadersAhead(fs::u8path(argv[2]));
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
    common::RotateLog("mua3", 5); // the previous run's log goes to logs/
    common::SetLogFile("mua3.log");
    LOG_INFO("Main", "MUA3 Native {}", common::Version());
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
    input.Init(cfg.input, cfg.camera_speed);
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
        PollCommandFile(&input);
        platform::bugcap::Poll(renderer, display);
        display.BeginFrame();
        display.EndFrame();
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
