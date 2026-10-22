#include "common/common.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <mutex>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace common {

static std::atomic<LogLevel> g_level{LogLevel::Info};
static std::mutex g_log_mutex;
static FILE* g_log_file = nullptr;
static const auto g_start = std::chrono::steady_clock::now();

void SetLogLevel(LogLevel level) { g_level = level; }
LogLevel GetLogLevel() { return g_level; }

void SetLogFile(const fs::path& path) {
    std::lock_guard lk(g_log_mutex);
    if (g_log_file)
        std::fclose(g_log_file);
    g_log_file = _wfopen(path.c_str(), L"w");
}

void LogRaw(LogLevel level, const char* category, std::string_view message) {
    static const char* names[] = {"TRACE", "DEBUG", "INFO ", "WARN ", "ERROR", "FATAL"};
    const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_start).count();
    const std::string line = fmt::format("[{:9.4f}] [{}] [{}:{}] {}\n", t, names[int(level)], category,
                                         GetCurrentThreadId(), message);
    std::lock_guard lk(g_log_mutex);
    std::fwrite(line.data(), 1, line.size(), stderr);
    if (g_log_file) {
        std::fwrite(line.data(), 1, line.size(), g_log_file);
        if (level >= LogLevel::Warn)
            std::fflush(g_log_file);
    }
}

void Panic(std::string_view message) {
    LogRaw(LogLevel::Fatal, "Panic", message);
    {
        std::lock_guard lk(g_log_mutex);
        if (g_log_file)
            std::fflush(g_log_file);
    }
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
