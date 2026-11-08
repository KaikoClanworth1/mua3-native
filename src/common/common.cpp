#include "common/common.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <fstream>
#include <mutex>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace common {

static std::atomic<LogLevel> g_level{LogLevel::Info};
static std::mutex g_log_mutex; // the writer (stderr and the log file)
static FILE* g_log_file = nullptr;
// Lines are queued and written by a background thread: a write to a busy hard disk (or a paused console) can block
// for most of a second, and every thread that logs (the renderer, the game's NVN thread) used to wait behind it.
// Errors and worse are written at once, after everything queued before them, so a crash leaves them on disk.
static std::mutex g_queue_mutex;
static std::condition_variable g_queue_cv;
static std::string g_queue;      // lines not yet written
static u64 g_dropped = 0;        // lines dropped while the queue was full
constexpr size_t kMaxQueued = 64u << 20;
static const auto g_start = std::chrono::steady_clock::now();

void SetLogLevel(LogLevel level) { g_level = level; }
LogLevel GetLogLevel() { return g_level; }

void SetLogFile(const fs::path& path) {
    std::lock_guard lk(g_log_mutex);
    if (g_log_file)
        std::fclose(g_log_file);
    g_log_file = _wfopen(path.c_str(), L"w");
}

// Writes everything queued so far.
static void FlushLog() {
    std::lock_guard wl(g_log_mutex);
    std::string text;
    u64 dropped;
    {
        std::lock_guard lk(g_queue_mutex);
        text.swap(g_queue);
        dropped = std::exchange(g_dropped, 0);
    }
    if (dropped)
        text += fmt::format("[log] {} lines dropped (writer behind)\n", dropped);
    if (text.empty())
        return;
    std::fwrite(text.data(), 1, text.size(), stderr);
    if (g_log_file) {
        std::fwrite(text.data(), 1, text.size(), g_log_file);
        std::fflush(g_log_file);
    }
}

static void StartLogWriter() {
    static const bool started = [] {
        std::thread([] {
            SetThreadDescription(GetCurrentThread(), L"LogWriter");
            for (;;) {
                {
                    std::unique_lock lk(g_queue_mutex);
                    g_queue_cv.wait(lk, [] { return !g_queue.empty() || g_dropped; });
                }
                FlushLog();
                Sleep(20); // gather a batch
            }
        }).detach();
        std::atexit(FlushLog);
        return true;
    }();
    (void)started;
}

void LogRaw(LogLevel level, const char* category, std::string_view message) {
    static const char* names[] = {"TRACE", "DEBUG", "INFO ", "WARN ", "ERROR", "FATAL"};
    const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_start).count();
    const std::string line = fmt::format("[{:9.4f}] [{}] [{}:{}] {}\n", t, names[int(level)], category,
                                         GetCurrentThreadId(), message);
    StartLogWriter();
    {
        std::lock_guard lk(g_queue_mutex);
        if (g_queue.size() + line.size() <= kMaxQueued)
            g_queue += line;
        else
            g_dropped++;
    }
    if (level >= LogLevel::Error)
        FlushLog();
    else
        g_queue_cv.notify_one();
}

void Panic(std::string_view message) {
    LogRaw(LogLevel::Fatal, "Panic", message); // (written at once)
    if (IsDebuggerPresent())
        __debugbreak();
    std::fflush(stderr);
    std::abort();
}

std::vector<u8> ReadFile(const fs::path& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f)
        return {};
    const auto size = static_cast<size_t>(f.tellg());
    std::vector<u8> data(size);
    f.seekg(0);
    f.read(reinterpret_cast<char*>(data.data()), size);
    return data;
}

bool WriteFile(const fs::path& path, std::span<const u8> data) {
    if (path.has_parent_path())
        fs::create_directories(path.parent_path());
    std::ofstream f(path, std::ios::binary);
    if (!f)
        return false;
    f.write(reinterpret_cast<const char*>(data.data()), data.size());
    return bool(f);
}

std::string ToHex(std::span<const u8> data) {
    static const char* digits = "0123456789abcdef";
    std::string s;
    s.reserve(data.size() * 2);
    for (u8 b : data) {
        s.push_back(digits[b >> 4]);
        s.push_back(digits[b & 15]);
    }
    return s;
}

std::optional<std::vector<u8>> FromHex(std::string_view hex) {
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    if (hex.size() % 2)
        return std::nullopt;
    std::vector<u8> out(hex.size() / 2);
    for (size_t i = 0; i < out.size(); i++) {
        const int hi = nib(hex[2 * i]), lo = nib(hex[2 * i + 1]);
        if (hi < 0 || lo < 0)
            return std::nullopt;
        out[i] = u8(hi << 4 | lo);
    }
    return out;
}

} // namespace common
