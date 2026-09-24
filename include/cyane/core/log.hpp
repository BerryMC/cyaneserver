#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>

namespace cyane::log {

enum class Level : std::uint8_t {
    trace,
    debug,
    info,
    warn,
    error,
    off,
};

[[nodiscard]] constexpr std::string_view to_string(Level level) noexcept {
    switch (level) {
        case Level::trace: return "TRACE";
        case Level::debug: return "DEBUG";
        case Level::info: return "INFO";
        case Level::warn: return "WARN";
        case Level::error: return "ERROR";
        case Level::off: return "OFF";
    }
    return "?";
}

[[nodiscard]] std::optional<Level> level_from_string(std::string_view name) noexcept;

struct Options {
    Level level{Level::info};
    std::string file;
    bool console{true};
};

struct Stats {
    std::uint64_t written{0};
    std::uint64_t dropped{0};
};

inline constexpr std::size_t kMaxText = 240;
inline constexpr std::size_t kMaxThreadName = 16;

struct Record {
    std::array<char, kMaxText> text{};
    std::array<char, kMaxThreadName> thread{};
    std::uint64_t wall_nanos{0};
    std::uint16_t len{0};
    Level level{Level::info};
};

namespace detail {
struct Scratch {
    char* data;
    std::ptrdiff_t size;
};
[[nodiscard]] Scratch tls_scratch() noexcept;
}

void start(const Options& options);
void stop() noexcept;
void flush() noexcept;

void set_level(Level level) noexcept;
[[nodiscard]] Level level() noexcept;
[[nodiscard]] Stats stats() noexcept;
void set_thread_name(std::string_view name) noexcept;

// 交互式控制台协调（可选）。before/after 在日志批量写 stdout 期间、持有输出锁时被调用：
// before 擦除当前输入行，after 重绘提示符与输入缓冲。钩子内不得再产生日志。
void set_console_hooks(void (*before)() noexcept, void (*after)() noexcept) noexcept;
// 获取/释放输出锁，供行编辑器回显时与日志刷新串行化（不得在持锁期间产生日志）。
void console_lock() noexcept;
void console_unlock() noexcept;

void write(Level level, std::string_view text) noexcept;

template <typename... Args>
void emit(Level lvl, std::format_string<Args...> fmt, Args&&... args) {
    const auto scratch = detail::tls_scratch();
    const auto result = std::format_to_n(scratch.data, scratch.size, fmt, std::forward<Args>(args)...);
    write(lvl, std::string_view{scratch.data, static_cast<std::size_t>(std::min(result.size, scratch.size))});
}

template <typename... Args>
void trace(std::format_string<Args...> fmt, Args&&... args) {
    if (level() > Level::trace) {
        return;
    }
    emit(Level::trace, fmt, std::forward<Args>(args)...);
}

template <typename... Args>
void debug(std::format_string<Args...> fmt, Args&&... args) {
    if (level() > Level::debug) {
        return;
    }
    emit(Level::debug, fmt, std::forward<Args>(args)...);
}

template <typename... Args>
void info(std::format_string<Args...> fmt, Args&&... args) {
    if (level() > Level::info) {
        return;
    }
    emit(Level::info, fmt, std::forward<Args>(args)...);
}

template <typename... Args>
void warn(std::format_string<Args...> fmt, Args&&... args) {
    if (level() > Level::warn) {
        return;
    }
    emit(Level::warn, fmt, std::forward<Args>(args)...);
}

template <typename... Args>
void error(std::format_string<Args...> fmt, Args&&... args) {
    if (level() > Level::error) {
        return;
    }
    emit(Level::error, fmt, std::forward<Args>(args)...);
}

}
