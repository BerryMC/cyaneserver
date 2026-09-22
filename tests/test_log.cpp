#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>

#include "cyane/core/log.hpp"
#include "test_framework.hpp"

namespace {

[[nodiscard]] std::filesystem::path fresh_log_path(std::string_view name) {
    auto path = std::filesystem::temp_directory_path() / std::string{name};
    std::filesystem::remove(path);
    return path;
}

[[nodiscard]] std::string slurp(const std::filesystem::path& path) {
    std::ifstream file{path, std::ios::binary};
    std::ostringstream contents;
    contents << file.rdbuf();
    return std::move(contents).str();
}

[[nodiscard]] bool has_line_containing(const std::string& text, std::string_view needle) {
    std::size_t pos = 0;
    while (pos < text.size()) {
        const auto end = text.find('\n', pos);
        const std::string_view line{text.data() + pos, (end == std::string::npos ? text.size() : end) - pos};
        if (line.find(needle) != std::string_view::npos) {
            return true;
        }
        if (end == std::string::npos) {
            break;
        }
        pos = end + 1;
    }
    return false;
}

}

CYANE_TEST(log_filters_below_configured_level) {
    const auto path = fresh_log_path("cyane-log-level.log");
    cyane::log::start(cyane::log::Options{.level = cyane::log::Level::warn, .file = path.string(), .console = false});

    cyane::log::info("visible-info");
    cyane::log::warn("kept-warn");
    cyane::log::debug("hidden-debug");
    cyane::log::flush();
    cyane::log::stop();

    const std::string text = slurp(path);
    CYANE_CHECK(has_line_containing(text, "kept-warn"));
    CYANE_CHECK(!has_line_containing(text, "visible-info"));
    CYANE_CHECK(!has_line_containing(text, "hidden-debug"));
    CYANE_CHECK(has_line_containing(text, "[WARN "));
}

CYANE_TEST(log_records_thread_name_and_level) {
    const auto path = fresh_log_path("cyane-log-thread.log");
    cyane::log::start(cyane::log::Options{.level = cyane::log::Level::trace, .file = path.string(), .console = false});
    cyane::log::set_thread_name("tick");
    cyane::log::info("tick-marker");
    cyane::log::flush();
    cyane::log::stop();

    const std::string text = slurp(path);
    CYANE_CHECK(has_line_containing(text, "tick-marker"));
    CYANE_CHECK(has_line_containing(text, "[tick"));
    CYANE_CHECK(has_line_containing(text, "[TRACE") == false);
}

CYANE_TEST(log_truncates_oversized_messages) {
    const auto path = fresh_log_path("cyane-log-truncate.log");
    cyane::log::start(cyane::log::Options{.level = cyane::log::Level::info, .file = path.string(), .console = false});
    const std::string huge(4096, 'x');
    cyane::log::info("{}", huge);
    cyane::log::flush();
    cyane::log::stop();

    const std::string text = slurp(path);
    CYANE_CHECK(text.size() > 0);
    CYANE_CHECK(text.size() < cyane::log::kMaxText + 128);
}

CYANE_TEST(log_counts_written_and_dropped) {
    cyane::log::start(cyane::log::Options{.level = cyane::log::Level::trace, .file = "", .console = false});
    cyane::log::info("counted");
    cyane::log::flush();
    const auto after_write = cyane::log::stats();
    CYANE_CHECK(after_write.written >= 1);
    CYANE_CHECK_EQ(after_write.dropped, std::uint64_t{0});
    cyane::log::stop();

    // 停止后无人排空队列：超出容量的记录必须计入 dropped 而不是阻塞调用方
    const auto before = cyane::log::stats();
    cyane::log::set_level(cyane::log::Level::trace);
    for (int i = 0; i < 8300; ++i) {
        cyane::log::info("overflow {}", i);
    }
    const auto after = cyane::log::stats();
    CYANE_CHECK(after.dropped - before.dropped >= 100);
    cyane::log::stop();
    cyane::log::set_level(cyane::log::Level::info);
}

CYANE_TEST(log_restart_reopens_stream) {
    const auto first = fresh_log_path("cyane-log-first.log");
    const auto second = fresh_log_path("cyane-log-second.log");

    cyane::log::start(cyane::log::Options{.level = cyane::log::Level::info, .file = first.string(), .console = false});
    cyane::log::info("into-first");
    cyane::log::stop();

    cyane::log::start(cyane::log::Options{.level = cyane::log::Level::info, .file = second.string(), .console = false});
    cyane::log::info("into-second");
    cyane::log::stop();

    CYANE_CHECK(has_line_containing(slurp(first), "into-first"));
    CYANE_CHECK(has_line_containing(slurp(second), "into-second"));
    CYANE_CHECK(!has_line_containing(slurp(second), "into-first"));
}
