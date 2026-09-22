#include <atomic>
#include <charconv>
#include <csignal>
#include <cstdint>
#include <print>
#include <string>
#include <string_view>

#include "cyane/core/config.hpp"
#include "cyane/core/error.hpp"
#include "cyane/core/log.hpp"
#include "cyane/game/server.hpp"
#include "cyane/generated/registry_meta.hpp"

namespace {

constexpr std::string_view kDefaultConfig = "server.toml";

std::atomic<cyane::Server*> g_server{nullptr};

extern "C" void on_signal(int) {
    if (auto* server = g_server.load(std::memory_order_relaxed); server != nullptr) {
        server->request_stop();
    }
}

void print_usage() {
    std::print("usage: cyane [--config <path>] [--ticks <n>] [--version] [--help]\n");
}

[[nodiscard]] bool parse_u64(std::string_view text, std::uint64_t& out) {
    const char* const begin = text.data();
    const char* const end = begin + text.size();
    const auto [ptr, ec] = std::from_chars(begin, end, out);
    return ec == std::errc{} && ptr == end;
}

}

int main(int argc, char** argv) {
    std::string config_path{kDefaultConfig};
    std::uint64_t max_ticks = 0;

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        if (arg == "--help" || arg == "-h") {
            print_usage();
            return 0;
        }
        if (arg == "--version" || arg == "-V") {
            std::print("cyaneserver {} | Minecraft {} (protocol {})\n",
                       cyane::kServerVersion,
                       cyane::generated::kMinecraftVersion,
                       cyane::generated::kProtocolVersion);
            return 0;
        }
        if (arg == "--config" && i + 1 < argc) {
            config_path = argv[++i];
            continue;
        }
        if (arg == "--ticks" && i + 1 < argc) {
            if (!parse_u64(argv[++i], max_ticks)) {
                std::print(stderr, "cyane: invalid --ticks value\n");
                return 2;
            }
            continue;
        }
        std::print(stderr, "cyane: unknown argument '{}'\n", arg);
        print_usage();
        return 2;
    }

    auto config = cyane::Config::load_file(config_path);
    if (!config) {
        std::print(stderr, "cyane: {}: {}\n", cyane::to_string(config.error().code), config.error().message);
        return 1;
    }

    auto server_config = cyane::ServerConfig::from(*config);
    if (!server_config) {
        std::print(stderr, "cyane: {}: {}\n", cyane::to_string(server_config.error().code), server_config.error().message);
        return 1;
    }

    const auto level = cyane::log::level_from_string(server_config->log_level).value_or(cyane::log::Level::info);
    cyane::log::start(cyane::log::Options{.level = level, .file = server_config->log_file});
    cyane::log::set_thread_name("main");
    std::atexit([] { cyane::log::stop(); });

    auto server = cyane::Server::create(std::move(*server_config));
    if (!server) {
        cyane::log::error("cannot start server: {}", server.error().message);
        return 1;
    }

    cyane::Server* const running = server->get();
    g_server.store(running, std::memory_order_relaxed);
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    const int code = running->run(max_ticks);
    g_server.store(nullptr, std::memory_order_relaxed);
    cyane::log::stop();
    return code;
}
