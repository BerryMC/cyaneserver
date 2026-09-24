#include "cyane/core/log.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <iterator>
#include <mutex>
#include <print>
#include <system_error>
#include <thread>

#include "cyane/core/mpmc_queue.hpp"
#include "cyane/core/time.hpp"

namespace cyane::log {
namespace {

constexpr std::size_t kQueueCapacity = 8192;
constexpr std::size_t kScratchSize = 1024;
constexpr std::size_t kFlushBatch = 1024;
constexpr std::size_t kOutBufferLimit = 1U << 16;
constexpr auto kIdleSleep = std::chrono::milliseconds{1};

struct State {
    MpmcQueue<Record> queue{kQueueCapacity};
    std::atomic<Level> level{Level::info};
    std::atomic<std::uint64_t> written{0};
    std::atomic<std::uint64_t> dropped{0};
    std::atomic<bool> running{false};
    std::mutex out_mutex;
    std::FILE* stream{nullptr};
    bool console{true};
    void (*console_before)() noexcept {nullptr};
    void (*console_after)() noexcept {nullptr};
    std::jthread flusher;
};

State& state() noexcept {
    static State instance;
    return instance;
}

std::uint64_t base_wall_nanos{0};
std::uint64_t base_steady_nanos{0};

[[nodiscard]] std::uint64_t wall_nanos_now() noexcept {
    const auto steady = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count());
    return base_wall_nanos + (steady - base_steady_nanos);
}

char* scratch_buffer() noexcept {
    thread_local std::array<char, kScratchSize> buffer{};
    return buffer.data();
}

std::array<char, kMaxThreadName>& thread_name_slot() noexcept {
    thread_local std::array<char, kMaxThreadName> name{'m', 'a', 'i', 'n'};
    return name;
}

void format_into(std::string& out, const Record& record) {
    const std::uint64_t total = record.wall_nanos;
    const auto seconds = static_cast<std::time_t>(total / 1'000'000'000ULL);
    const auto millis = (total / 1'000'000ULL) % 1000ULL;
    std::tm tm{};
    localtime_r(&seconds, &tm);
    std::format_to(std::back_inserter(out),
                   "{:04}-{:02}-{:02} {:02}:{:02}:{:02}.{:03} [{:<5}] [{:<7}] {}\n",
                   tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec,
                   millis, to_string(record.level), std::string_view{record.thread.data()},
                   std::string_view{record.text.data(), record.len});
}

void write_out(State& s, std::string& buffer) {
    if (buffer.empty()) {
        return;
    }
    const std::lock_guard<std::mutex> lock{s.out_mutex};
    if (s.stream != nullptr) {
        std::fwrite(buffer.data(), 1, buffer.size(), s.stream);
        std::fflush(s.stream);
    }
    if (s.console) {
        if (s.console_before != nullptr) {
            s.console_before();
        }
        std::fwrite(buffer.data(), 1, buffer.size(), stdout);
        std::fflush(stdout);
        if (s.console_after != nullptr) {
            s.console_after();
        }
    }
    buffer.clear();
}

[[nodiscard]] bool drain(State& s, std::string& buffer, Record& record) noexcept {
    bool drained = false;
    for (std::size_t i = 0; i < kFlushBatch; ++i) {
        if (!s.queue.try_pop(record)) {
            return drained;
        }
        format_into(buffer, record);
        s.written.fetch_add(1, std::memory_order_relaxed);
        drained = true;
    }
    return true;
}

void flusher_loop(State& s) noexcept {
    std::string buffer;
    buffer.reserve(kOutBufferLimit + 1024);
    Record record;
    while (s.running.load(std::memory_order_acquire)) {
        const bool progressed = drain(s, buffer, record);
        if (buffer.size() >= kOutBufferLimit) {
            write_out(s, buffer);
        }
        if (!progressed) {
            write_out(s, buffer);
            std::this_thread::sleep_for(kIdleSleep);
        }
    }
    while (drain(s, buffer, record)) {
    }
    write_out(s, buffer);
}

}

std::optional<Level> level_from_string(std::string_view name) noexcept {
    if (name == "trace") return Level::trace;
    if (name == "debug") return Level::debug;
    if (name == "info") return Level::info;
    if (name == "warn" || name == "warning") return Level::warn;
    if (name == "error") return Level::error;
    if (name == "off") return Level::off;
    return std::nullopt;
}

namespace detail {

Scratch tls_scratch() noexcept {
    return Scratch{scratch_buffer(), static_cast<std::ptrdiff_t>(kScratchSize)};
}

}

void start(const Options& options) {
    State& s = state();
    stop();

    base_wall_nanos = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count());
    base_steady_nanos = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count());

    s.level.store(options.level, std::memory_order_relaxed);
    s.console = options.console;
    {
        const std::lock_guard<std::mutex> lock{s.out_mutex};
        if (s.stream != nullptr) {
            std::fclose(s.stream);
            s.stream = nullptr;
        }
        if (!options.file.empty()) {
            const std::filesystem::path path{options.file};
            if (path.has_parent_path()) {
                std::error_code error;
                std::filesystem::create_directories(path.parent_path(), error);
            }
            s.stream = std::fopen(options.file.c_str(), "ab");
            if (s.stream == nullptr) {
                std::print(stderr, "cyane: cannot open log file '{}'\n", options.file);
            }
        }
    }
    s.written.store(0, std::memory_order_relaxed);
    s.dropped.store(0, std::memory_order_relaxed);
    s.running.store(true, std::memory_order_release);
    s.flusher = std::jthread{[&s] { flusher_loop(s); }};
}

void stop() noexcept {
    State& s = state();
    const bool was_running = s.running.exchange(false, std::memory_order_acq_rel);
    if (was_running && s.flusher.joinable()) {
        s.flusher.join();
    }
    std::string buffer;
    Record record;
    while (drain(s, buffer, record)) {
    }
    write_out(s, buffer);
    const std::lock_guard<std::mutex> lock{s.out_mutex};
    if (s.stream != nullptr) {
        std::fclose(s.stream);
        s.stream = nullptr;
    }
}

void flush() noexcept {
    State& s = state();
    std::string buffer;
    Record record;
    while (drain(s, buffer, record)) {
    }
    write_out(s, buffer);
}

void set_level(Level lvl) noexcept { state().level.store(lvl, std::memory_order_relaxed); }

Level level() noexcept { return state().level.load(std::memory_order_relaxed); }

Stats stats() noexcept {
    State& s = state();
    return Stats{s.written.load(std::memory_order_relaxed), s.dropped.load(std::memory_order_relaxed)};
}

void set_thread_name(std::string_view name) noexcept {
    auto& slot = thread_name_slot();
    const std::size_t count = std::min(name.size(), kMaxThreadName - 1);
    std::memcpy(slot.data(), name.data(), count);
    std::fill(slot.begin() + static_cast<std::ptrdiff_t>(count), slot.end(), '\0');
}

void set_console_hooks(void (*before)() noexcept, void (*after)() noexcept) noexcept {
    State& s = state();
    const std::lock_guard<std::mutex> lock{s.out_mutex};
    s.console_before = before;
    s.console_after = after;
}

void console_lock() noexcept { state().out_mutex.lock(); }
void console_unlock() noexcept { state().out_mutex.unlock(); }

void write(Level lvl, std::string_view text) noexcept {
    Record record;
    const std::size_t count = std::min(text.size(), kMaxText);
    std::memcpy(record.text.data(), text.data(), count);
    record.len = static_cast<std::uint16_t>(count);
    record.level = lvl;
    record.wall_nanos = wall_nanos_now();
    const auto& name = thread_name_slot();
    std::memcpy(record.thread.data(), name.data(), kMaxThreadName);

    State& s = state();
    if (!s.queue.try_push(std::move(record))) {
        s.dropped.fetch_add(1, std::memory_order_relaxed);
    }
}

}
