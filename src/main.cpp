#include <atomic>
#include <cerrno>
#include <charconv>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <print>
#include <string>
#include <string_view>
#include <thread>

#include <poll.h>
#include <termios.h>
#include <unistd.h>

#include <sys/eventfd.h>

#include "cyane/core/config.hpp"
#include "cyane/core/error.hpp"
#include "cyane/core/log.hpp"
#include "cyane/game/server.hpp"
#include "cyane/generated/registry_meta.hpp"

namespace {

constexpr std::string_view kDefaultConfig = "config/server.toml";
constexpr std::string_view kPrompt = "\033[36m> \033[0m";

// ANSI 颜色
constexpr std::string_view kReset  = "\033[0m";
constexpr std::string_view kGreen  = "\033[32m";
constexpr std::string_view kYellow = "\033[33m";
constexpr std::string_view kCyan   = "\033[36m";
constexpr std::string_view kRed    = "\033[31m";
constexpr std::string_view kBold   = "\033[1m";
constexpr std::string_view kGray   = "\033[90m";

// 交互式行编辑器与日志刷新共享的输入行状态。
// 所有访问都在 log::console_lock() 保护下进行（钩子由 flusher 持锁时调用）。
std::string g_input_line;
bool g_prompt_active = false;

// 日志写出前擦除当前输入行
void console_before_log() noexcept {
    if (g_prompt_active) {
        std::fputs("\r\033[K", stdout);
    }
}

// 日志写出后重绘提示符与已输入内容
void console_after_log() noexcept {
    if (g_prompt_active) {
        std::fwrite(kPrompt.data(), 1, kPrompt.size(), stdout);
        std::fwrite(g_input_line.data(), 1, g_input_line.size(), stdout);
        std::fflush(stdout);
    }
}

// 确保配置目录存在；若目录创建失败则写入日志但不致命
void ensure_config_dir(std::string_view config_path) {
    const std::filesystem::path p{config_path};
    if (p.has_parent_path()) {
        std::error_code ec;
        std::filesystem::create_directories(p.parent_path(), ec);
        if (ec) {
            std::cerr << "cyane: warning: could not create config dir: " << ec.message() << "\n";
        }
    }
}

std::atomic<cyane::Server*> g_server{nullptr};
// 交互式控制台唤醒事件：SIGINT 时写入，让阻塞在 poll 的行编辑线程退出并恢复终端模式
std::atomic<int> g_console_wake_fd{-1};

extern "C" void on_signal(int) {
    if (auto* server = g_server.load(std::memory_order_relaxed); server != nullptr) {
        server->request_stop();
    }
    // eventfd 写是异步信号安全的；线程醒来后自行恢复 termios 并退出
    const int fd = g_console_wake_fd.load(std::memory_order_relaxed);
    if (fd >= 0) {
        const std::uint64_t one{1};
        const auto written = ::write(fd, &one, sizeof(one));
        (void)written;
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
    const auto space = line.find(' ');
    const std::string_view cmd = line.substr(0, space);
    const std::string_view rest = space == std::string_view::npos ? std::string_view{} : trim(line.substr(space + 1));

    if (cmd == "help" || cmd == "?") {
        std::print("{}命令列表:{}\n", kBold, kReset);
        std::print("  {}help{}            显示此帮助\n", kCyan, kReset);
        std::print("  {}tps{}             显示当前 TPS 与在线人数\n", kCyan, kReset);
        std::print("  {}list{}            显示在线玩家列表\n", kCyan, kReset);
        std::print("  {}say{} <消息>      以服务器身份向所有玩家广播\n", kCyan, kReset);
        std::print("  {}kill{} <玩家名>   杀死指定在线玩家\n", kCyan, kReset);
        std::print("  {}gamemode{} <模式> <玩家名>  切换游戏模式\n", kCyan, kReset);
        std::print("  {}op{} <玩家名>     将玩家设为 OP\n", kCyan, kReset);
        std::print("  {}deop{} <玩家名>   撤销玩家 OP\n", kCyan, kReset);
        std::print("  {}save{}            立即保存世界存档\n", kCyan, kReset);
        std::print("  {}stop{}            停止服务器\n", kCyan, kReset);
        return true;
    }
    if (cmd == "tps") {
        std::print("{}TPS:{} {:.1f} {}|{} {}online:{} {}\n",
                   kGreen, kReset, server.current_tps(),
                   kGray, kReset,
                   kGreen, kReset, server.online_players());
        return true;
    }
    if (cmd == "list") {
        const auto names = server.player_names();
        std::print("{}online ({}):{}", kGreen, names.size(), kReset);
        for (const auto& name : names) {
            std::print(" {}{}{}", kCyan, name, kReset);
        }
        std::print("\n");
        return true;
    }
    if (cmd == "say") {
        if (rest.empty()) {
            std::print("{}usage:{} say <message>\n", kYellow, kReset);
            return true;
        }
        const std::string message = std::format("[Server] {}", rest);
        server.broadcast_system_message(message);
        std::print("{}[Server]{} {}\n", kYellow, kReset, rest);
        return true;
    }
    if (cmd == "kill") {
        if (rest.empty()) {
            std::print("{}usage:{} kill <player>\n", kYellow, kReset);
            return true;
        }
        if (server.kill_player_by_name(rest)) {
            std::print("{}killed{} {}\n", kRed, kReset, rest);
        } else {
            std::print("{}player not found:{} {}\n", kRed, kReset, rest);
        }
        return true;
    }
    if (cmd == "gamemode") {
        const auto space2 = rest.find(' ');
        const std::string_view mode = rest.substr(0, space2);
        const std::string_view target = space2 == std::string_view::npos ? std::string_view{} : trim(rest.substr(space2 + 1));
        if (mode.empty() || target.empty()) {
            std::print("{}usage:{} gamemode <mode> <player>\n", kYellow, kReset);
            return true;
        }
        if (server.set_player_gamemode(target, mode)) {
            std::print("{}{}{} -> {}{}{}\n", kCyan, target, kReset, kGreen, mode, kReset);
        } else {
            std::print("{}player not found or invalid mode:{} {}\n", kRed, kReset, target);
        }
        return true;
    }
    if (cmd == "op") {
        if (rest.empty()) {
            std::print("{}usage:{} op <player>\n", kYellow, kReset);
            return true;
        }
        if (server.op_player(rest)) {
            std::print("{}opped{} {}\n", kGreen, kReset, rest);
        } else {
            std::print("{}player not found:{} {}\n", kRed, kReset, rest);
        }
        return true;
    }
    if (cmd == "deop") {
        if (rest.empty()) {
            std::print("{}usage:{} deop <player>\n", kYellow, kReset);
            return true;
        }
        if (server.deop_player(rest)) {
            std::print("{}deopped{} {}\n", kGreen, kReset, rest);
        } else {
            std::print("{}player not found or not op:{} {}\n", kRed, kReset, rest);
        }
        return true;
    }
    if (cmd == "save") {
        server.save_world_now();
        std::print("{}world saved{}\n", kGreen, kReset);
        return true;
    }
    if (cmd == "stop") {
        std::print("{}stopping server...{}\n", kYellow, kReset);
        return false;
    }
    std::print("{}unknown command:{} {} {}(try 'help'){}\n", kRed, kReset, cmd, kGray, kReset);
    return true;
}

// 重绘当前输入行（调用方须持有 console_lock）
void redraw_input_locked() noexcept {
    std::fputs("\r\033[K", stdout);
    std::fwrite(kPrompt.data(), 1, kPrompt.size(), stdout);
    std::fwrite(g_input_line.data(), 1, g_input_line.size(), stdout);
    std::fflush(stdout);
}

// 原始模式下的交互式行编辑：提示符、逐字符回显、退格；日志滚动时自动重绘
void run_line_editor(cyane::Server& server) {
    termios original{};
    if (::tcgetattr(STDIN_FILENO, &original) != 0) {
        return;
    }
    termios raw = original;
    raw.c_lflag &= ~static_cast<tcflag_t>(ICANON | ECHO);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    ::tcsetattr(STDIN_FILENO, TCSANOW, &raw);

    cyane::log::console_lock();
    g_prompt_active = true;
    redraw_input_locked();
    cyane::log::console_unlock();

    // stdin + 唤醒事件 fd：Ctrl+C/SIGTERM 写事件 fd 使 poll 返回，
    // 线程得以走完 termios 恢复再退出（阻塞态直接 detach 会把终端留在 raw 模式）
    const int wake_fd = g_console_wake_fd.load(std::memory_order_relaxed);
    const nfds_t nfds = wake_fd >= 0 ? 2 : 1;
    pollfd pfds[2]{};
    pfds[0].fd = STDIN_FILENO;
    pfds[0].events = POLLIN;
    if (wake_fd >= 0) {
        pfds[1].fd = wake_fd;
        pfds[1].events = POLLIN;
    }

    while (true) {
        const int ready = ::poll(pfds, nfds, -1);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (wake_fd >= 0 && (pfds[1].revents & POLLIN) != 0) {
            // 信号触发的唤醒：退出循环并恢复终端
            std::uint64_t drained = 0;
            while (::read(wake_fd, &drained, sizeof(drained)) > 0) {
            }
            break;
        }
        if ((pfds[0].revents & POLLIN) == 0) {
            continue;
        }
        char ch = 0;
        if (::read(STDIN_FILENO, &ch, 1) != 1) {
            break;
        }
        if (ch == '\n' || ch == '\r') {
            cyane::log::console_lock();
            std::fputc('\n', stdout);
            std::fflush(stdout);
            std::string line = std::move(g_input_line);
            g_input_line.clear();
            cyane::log::console_unlock();

            if (!handle_console_command(server, line)) {
                server.request_stop();
                break;
            }
            cyane::log::console_lock();
            redraw_input_locked();
            cyane::log::console_unlock();
        } else if (ch == 0x7f || ch == 0x08) {  // 退格
            cyane::log::console_lock();
            if (!g_input_line.empty()) {
                // 按字节回删；命令均为 ASCII，够用
                g_input_line.pop_back();
                redraw_input_locked();
            }
            cyane::log::console_unlock();
        } else if (ch == 0x03) {  // 个别终端未启用 ISIG 时 ^C 作为字节到达
            server.request_stop();
            break;
        } else if (ch == 0x04) {  // Ctrl+D
            if (g_input_line.empty()) {
                server.request_stop();
                break;
            }
        } else if (static_cast<unsigned char>(ch) >= 0x20) {  // 可打印字符
            cyane::log::console_lock();
            g_input_line.push_back(ch);
            std::fputc(ch, stdout);
            std::fflush(stdout);
            cyane::log::console_unlock();
        }
    }

    cyane::log::console_lock();
    g_prompt_active = false;
    std::fputs("\r\033[K", stdout);
    std::fflush(stdout);
    cyane::log::console_unlock();
    ::tcsetattr(STDIN_FILENO, TCSANOW, &original);
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

    ensure_config_dir(config_path);
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

    // 启动横幅
    std::print("{}{}\n", kCyan, kBold);
    std::print("   ______                       \n");
    std::print("  / ____/_ _____ _____  ___     \n");
    std::print(" / /   / // / _ `/ _ \\/ -_)    \n");
    std::print(" \\____/\\_, /\\_,_/_//_/\\__/  {}CyaneServer {}{}\n",
               "", kReset, cyane::kServerVersion);
    std::print("{}     /___/                    {}Minecraft {} (protocol {}){}\n",
               kCyan, kGray, cyane::generated::kMinecraftVersion,
               cyane::generated::kProtocolVersion, kReset);
    std::print("\n");

    // 交互式控制台读取放到后台线程；tick 循环留在主线程，
    // 这样 SIGINT 触发 request_stop 后 run() 立即返回并退出进程。
    const bool interactive = max_ticks == 0 && ::isatty(STDIN_FILENO) != 0;
    std::thread console;
    if (interactive) {
        cyane::log::set_console_hooks(console_before_log, console_after_log);
        const int wake_fd = ::eventfd(0, EFD_NONBLOCK);
        g_console_wake_fd.store(wake_fd, std::memory_order_relaxed);
        console = std::thread{[running] { run_line_editor(*running); }};
    } else if (max_ticks == 0) {
        // 非 TTY（管道/重定向）：退回逐行读取，无提示符与行编辑
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
    cyane::log::set_console_hooks(nullptr, nullptr);
    // 交互式线程经 eventfd 唤醒后自行恢复终端模式并退出，可安全 join
    if (console.joinable()) {
        console.join();
    }
    cyane::log::stop();
    return code;
}

