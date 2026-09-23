#include <atomic>
#include <charconv>
#include <csignal>
#include <cstdint>
#include <iostream>
#include <print>
#include <string>
#include <string_view>
#include <thread>

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

// 去掉首尾空白
[[nodiscard]] std::string_view trim(std::string_view text) noexcept {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) {
        return {};
    }
    const auto last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

// 处理一条控制台命令，返回 false 表示需要停止服务器
[[nodiscard]] bool handle_console_command(cyane::Server& server, std::string_view line) {
    line = trim(line);
    if (line.empty()) {
        return true;
    }
    // 命令词 = 第一个空格前
    const auto space = line.find(' ');
    const std::string_view cmd = line.substr(0, space);
    const std::string_view rest = space == std::string_view::npos ? std::string_view{} : trim(line.substr(space + 1));

    if (cmd == "help") {
        std::print("commands:\n");
        std::print("  help            显示此帮助\n");
        std::print("  tps             显示当前 TPS 与在线人数\n");
        std::print("  say <消息>      以服务器身份向所有玩家广播\n");
        std::print("  kill <玩家名>   杀死指定在线玩家\n");
        std::print("  stop            停止服务器\n");
        return true;
    }
    if (cmd == "tps") {
        std::print("TPS: {:.1f} | online: {}\n", server.current_tps(), server.online_players());
        return true;
    }
    if (cmd == "say") {
        if (rest.empty()) {
            std::print("usage: say <message>\n");
            return true;
        }
        const std::string message = std::format("[Server] {}", rest);
        server.broadcast_system_message(message);
        std::print("{}\n", message);
        return true;
    }
    if (cmd == "kill") {
        if (rest.empty()) {
            std::print("usage: kill <player>\n");
            return true;
        }
        if (server.kill_player_by_name(rest)) {
            std::print("killed {}\n", rest);
        } else {
            std::print("player not found: {}\n", rest);
        }
        return true;
    }
    if (cmd == "stop") {
        std::print("stopping server...\n");
        return false;
    }
    std::print("unknown command: {} (try 'help')\n", cmd);
    return true;
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

    // 交互式控制台读取放到后台线程；tick 循环留在主线程，
    // 这样 SIGINT 触发 request_stop 后 run() 立即返回并退出进程，
    // 不会被卡在 std::getline 上的读取线程阻塞。
    std::thread console;
    if (max_ticks == 0) {
        console = std::thread{[running] {
            std::string line;
            while (std::getline(std::cin, line)) {
                if (!handle_console_command(*running, line)) {
                    running->request_stop();
                    return;
                }
            }
        }};
    }

    const int code = running->run(max_ticks);

    g_server.store(nullptr, std::memory_order_relaxed);
    // 读取线程可能仍阻塞在 getline 上，无法唤醒，直接 detach 让进程退出
    if (console.joinable()) {
        console.detach();
    }
    cyane::log::stop();
    return code;
}

