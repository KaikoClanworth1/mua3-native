#include "platform/bug_capture.h"

#include "gpu/d3d11/frame_capture.h"
#include "gpu/d3d11/renderer_d3d11.h"
#include "common/crash.h"
#include "core/system.h"
#include "platform/platform.h"

#include <atomic>
#include <chrono>
#include <fstream>
#include <mutex>
#include <regex>
#include <sstream>
#include <thread>

#include <imgui.h>

namespace platform::bugcap {
namespace {

std::atomic<bool> g_enabled{false}, g_requested{false}, g_busy{false};
fs::path g_save_dir;
std::mutex g_toast_mutex;
std::string g_toast;
std::chrono::steady_clock::time_point g_toast_until;
constexpr u64 kFrameBudget = 512ull << 20; // textures, buffers and target images of the frame capture

void Toast(std::string text, int seconds = 4) {
    std::lock_guard lk(g_toast_mutex);
    g_toast = std::move(text);
    g_toast_until = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
}

std::string ReadShared(const fs::path& path) { // the log stays open for writing
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

double LineTime(const std::string& line) { // "[  123.4567] ..." -> seconds since start, -1 if none
    if (line.size() < 3 || line[0] != '[')
        return -1;
    return std::atof(line.c_str() + 1);
}

// Copies the game's save files (read only: the source is never opened for writing).
std::string CopySave(const fs::path& dir) {
    std::string out;
    std::error_code ec;
    const fs::path src = g_save_dir / "account", dst = dir / "save";
    fs::create_directories(dst, ec);
    for (const auto& e : fs::directory_iterator(src, ec)) {
        if (!e.is_regular_file())
            continue;
        std::error_code ce;
        fs::copy_file(e.path(), dst / e.path().filename(), fs::copy_options::overwrite_existing, ce);
        out += fmt::format("  {} ({} bytes){}\n", e.path().filename().string(), e.file_size(ec), ce ? " COPY FAILED: " + ce.message() : "");
    }
    if (out.empty())
        out = fmt::format("  (no save files in {})\n", src.string());
    // savedatainfo: byte 0x16 chapter, 0x1c date (s16 year, then month, day, hour, minute)
    const auto info = common::ReadFile(dst / "savedatainfo");
    if (info.size() >= 0x24) {
        s16 year;
        std::memcpy(&year, &info[0x1c], 2);
        out += fmt::format("  chapter {}, saved {:04}-{:02}-{:02} {:02}:{:02}\n", info[0x16], year, info[0x1e], info[0x1f],
                           info[0x20], info[0x21]);
    }
    return out;
}

// Where the player is, from what the game loaded (romfs opens in this run's log).
std::string Location(const std::string& log) {
    static const std::regex stage_re(R"(open (STAGE/(ST\d+)/ST\d+_\d+)\.pld)"), scen_re(R"(open (SCENARIO/\S+))"),
        chara_re(R"(open Effect/(\d{3}_\w+)\.pefc)");
    std::string stage, stage_id, scenario;
    std::vector<std::string> chars;
    std::istringstream ls(log);
    for (std::string line; std::getline(ls, line);) {
        std::smatch m;
        if (line.find("open ") == std::string::npos)
            continue;
        if (std::regex_search(line, m, stage_re)) {
            stage = m[1], stage_id = m[2];
        } else if (std::regex_search(line, m, scen_re)) {
            scenario = m[1];
        } else if (std::regex_search(line, m, chara_re) && std::find(chars.begin(), chars.end(), m[1].str()) == chars.end()) {
            chars.push_back(m[1]);
        }
    }
    std::string out = fmt::format("stage: {}\nstage file: {}\nlast scenario file: {}\ncharacters loaded this run (the "
                                  "party, bosses and summons among them; packs stay loaded):",
                                  stage_id.empty() ? "(none loaded yet: title or menus)" : stage_id, stage, scenario);
    for (auto& c : chars)
        out += " " + c;
    return out + "\n";
}

void Run(fs::path dir, std::unique_ptr<gpu::FrameCapture> cap, gpu::D3D11Renderer* renderer, std::string started) {
    LOG_INFO("BugCapture", "capturing to {}", dir.string());
    const std::string save = CopySave(dir);
    // the frame capture is written by the renderer at the second present from now
    std::string frame = "not finished (no game frames presented within 15 s: loading or paused?)";
    for (int i = 0; i < 1500; i++) {
        if (cap->Done() && renderer->capture_.load() == nullptr) {
            frame = cap->Summary();
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (renderer->capture_.load() == cap.get()) { // never finished: take it back (under the renderer's lock)
        std::lock_guard lk(gpu::ContextMutex());
        renderer->capture_ = nullptr;
    }
    LOG_INFO("BugCapture", "frame: {}", frame);
    std::this_thread::sleep_for(std::chrono::milliseconds(200)); // the log writer catches up
    const std::string log = ReadShared("mua3.log");
    {   // last 30 s of the log
        std::vector<std::string> lines;
        std::istringstream ls(log);
        for (std::string line; std::getline(ls, line);)
            lines.push_back(std::move(line));
        double last = -1;
        for (auto it = lines.rbegin(); it != lines.rend() && last < 0; ++it)
            last = LineTime(*it);
        size_t from = lines.size();
        while (from > 0) {
            const double t = LineTime(lines[from - 1]);
            if (t >= 0 && t < last - 30)
                break;
            from--;
        }
        std::ofstream f(dir / "log_last_30s.txt", std::ios::binary);
        for (size_t i = from; i < lines.size(); i++)
            f << lines[i] << "\n";
    }
    const auto& cfg = core::Sys().Cfg();
    std::ofstream(dir / "info.txt")
        << "MUA3 Native bug capture\n"
        << "captured: " << started << "\n"
        << "build: " << common::Version() << "\n"
        << Location(log)
        << "save files (copied from " << (g_save_dir / "account").string() << ", the game's latest save; load with "
        << "inst.ps1 tocapture <i> <this folder>):\n" << save
        << "frame capture: " << frame << "\n"
        << "settings: fps_target " << cfg.fps_target << ", resolution_scale " << cfg.resolution_scale << ", anti_aliasing "
        << cfg.anti_aliasing << ", docked " << cfg.docked << "\n"
        << "party position: not known yet (the stage and characters above come from the files the game loaded)\n";
    Toast(fmt::format("Capture saved: bug_captures/{}", dir.filename().string()));
    LOG_INFO("BugCapture", "capture saved to {}", dir.string());
    g_busy = false;
}

} // namespace

void Init(bool enabled, const fs::path& save_dir) {
    g_enabled = enabled;
    g_save_dir = save_dir;
    if (enabled)
        LOG_INFO("BugCapture", "F4 bug capture on ([debug] bug_capture=1): folders go to bug_captures/");
}

bool Enabled() { return g_enabled; }

void Request() {
    if (g_enabled)
        g_requested = true;
}

void Poll(gpu::D3D11Renderer& renderer, Display& display) {
    if (!g_requested.exchange(false))
        return;
    if (g_busy.exchange(true)) {
        Toast("Bug capture still being written", 2);
        return;
    }
    const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm{};
    localtime_s(&tm, &now);
    char name[32], when[32];
    std::strftime(name, sizeof(name), "%Y-%m-%d_%H%M%S", &tm);
    std::strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", &tm);
    const fs::path dir = fs::path("bug_captures") / name;
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) {
        Toast("Bug capture failed: cannot create " + dir.string());
        g_busy = false;
        return;
    }
    auto cap = std::make_unique<gpu::FrameCapture>(dir, kFrameBudget);
    renderer.capture_ = cap.get();
    display.capture_path = dir / "window.bmp"; // the window as shown (before the toast appears in it)
    std::thread(Run, dir, std::move(cap), &renderer, std::string(when)).detach();
}

void DrawToast() {
    std::string text;
    {
        std::lock_guard lk(g_toast_mutex);
        if (g_toast.empty() || std::chrono::steady_clock::now() > g_toast_until)
            return;
        text = g_toast;
    }
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.08f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowBgAlpha(0.75f);
    ImGui::Begin("##bugcap", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
    ImGui::TextUnformatted(text.c_str());
    ImGui::End();
}

} // namespace platform::bugcap
