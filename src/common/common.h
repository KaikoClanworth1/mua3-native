#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <fmt/format.h>

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using s8 = std::int8_t;
using s16 = std::int16_t;
using s32 = std::int32_t;
using s64 = std::int64_t;
using f32 = float;
using f64 = double;

namespace fs = std::filesystem;

namespace common {

enum class LogLevel { Trace, Debug, Info, Warn, Error, Fatal };

void SetLogLevel(LogLevel level);
LogLevel GetLogLevel();
void SetLogFile(const fs::path& path);
void LogRaw(LogLevel level, const char* category, std::string_view message);

template <typename... Args>
inline void Log(LogLevel level, const char* category, fmt::format_string<Args...> f, Args&&... args) {
    if (level < GetLogLevel())
        return;
    LogRaw(level, category, fmt::format(f, std::forward<Args>(args)...));
}

[[noreturn]] void Panic(std::string_view message);

std::vector<u8> ReadFile(const fs::path& path);
bool WriteFile(const fs::path& path, std::span<const u8> data);
std::string ToHex(std::span<const u8> data);
std::optional<std::vector<u8>> FromHex(std::string_view hex);

template <typename T>
inline T Read(const void* p) {
    T v;
    std::memcpy(&v, p, sizeof(T));
    return v;
}

constexpr u64 AlignUp(u64 v, u64 a) { return (v + a - 1) & ~(a - 1); }

// Hash for integer keys (addresses, precomputed hashes): MSVC's std::hash<u64> runs FNV-1a over the bytes,
// which shows up in profiles of the renderer's per-draw lookups.
struct U64Hash {
    size_t operator()(u64 x) const {
        x ^= x >> 33;
        x *= 0xff51afd7ed558ccdull;
        x ^= x >> 33;
        return size_t(x);
    }
};
template <typename V>
using U64Map = std::unordered_map<u64, V, U64Hash>;
constexpr u64 AlignDown(u64 v, u64 a) { return v & ~(a - 1); }

} // namespace common

#define LOG_TRACE(cat, ...) ::common::Log(::common::LogLevel::Trace, cat, __VA_ARGS__)
#define LOG_DEBUG(cat, ...) ::common::Log(::common::LogLevel::Debug, cat, __VA_ARGS__)
#define LOG_INFO(cat, ...) ::common::Log(::common::LogLevel::Info, cat, __VA_ARGS__)
#define LOG_WARN(cat, ...) ::common::Log(::common::LogLevel::Warn, cat, __VA_ARGS__)
#define LOG_ERROR(cat, ...) ::common::Log(::common::LogLevel::Error, cat, __VA_ARGS__)
#define PANIC(...) ::common::Panic(fmt::format(__VA_ARGS__))
#define ASSERT(cond)                                                                    \
    do {                                                                                \
        if (!(cond))                                                                    \
            ::common::Panic(fmt::format("Assertion failed: {} ({}:{})", #cond, __FILE__, __LINE__)); \
    } while (0)
