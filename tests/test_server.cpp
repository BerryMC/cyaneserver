#include <chrono>
#include <cstdint>
#include <thread>
#include <utility>

#include "cyane/core/config.hpp"
#include "cyane/game/command.hpp"
#include "cyane/game/server.hpp"
#include "test_framework.hpp"

namespace {

[[nodiscard]] cyane::Result<cyane::ServerConfig> fast_config() {
    auto config = cyane::Config::parse(
        "[network]\nport = 0\n[server]\ntick_rate = 200\nworker_threads = 2\n[log]\nfile = \"\"\n");
    if (!config) {
        return std::unexpected{std::move(config.error())};
    }
    return cyane::ServerConfig::from(*config);
}

}

CYANE_TEST(server_runs_requested_number_of_ticks) {
    auto settings = fast_config();
    CYANE_CHECK(settings.has_value());
    if (!settings) {
        return;
    }
    CYANE_CHECK_EQ(settings->tick_rate, 200);
    CYANE_CHECK_EQ(settings->worker_threads, 2U);

    auto server = cyane::Server::create(std::move(*settings));
    CYANE_CHECK(server.has_value());
    if (!server) {
        return;
    }

    const int code = server->get()->run(60);
    CYANE_CHECK_EQ(code, 0);
    CYANE_CHECK_EQ(server->get()->stats().total_ticks(), std::uint64_t{60});
}

CYANE_TEST(server_stops_when_requested) {
    auto settings = fast_config();
    CYANE_CHECK(settings.has_value());
    if (!settings) {
        return;
    }
    auto server = cyane::Server::create(std::move(*settings));
    CYANE_CHECK(server.has_value());
    if (!server) {
        return;
    }

    cyane::Server* const raw = server->get();
    std::jthread stopper{[raw] {
        std::this_thread::sleep_for(std::chrono::milliseconds{120});
        raw->request_stop();
    }};

    const int code = raw->run(4000);
    CYANE_CHECK_EQ(code, 0);
    CYANE_CHECK(raw->stats().total_ticks() > 0);
    CYANE_CHECK(raw->stats().total_ticks() < 2000);
}

class TestSender : public cyane::game::CommandSender {
public:
    explicit TestSender(std::uint8_t op, bool is_p = false, std::string_view n = "Tester")
        : op_{op}, is_player_{is_p}, name_{n} {}

    [[nodiscard]] std::string_view name() const noexcept override { return name_; }
    [[nodiscard]] bool is_player() const noexcept override { return is_player_; }
    [[nodiscard]] std::uint8_t op_level() const noexcept override { return op_; }
    void send_feedback(std::string_view message, bool is_err = false) override {
        messages.push_back(std::string{message});
        if (is_err) errors.push_back(std::string{message});
    }

    std::vector<std::string> messages;
    std::vector<std::string> errors;

private:
    std::uint8_t op_{0};
    bool is_player_{false};
    std::string_view name_{"Tester"};
};

CYANE_TEST(command_dispatcher_executes_unified_commands) {
    auto settings = fast_config();
    CYANE_CHECK(settings.has_value());
    auto server = cyane::Server::create(std::move(*settings));
    CYANE_CHECK(server.has_value());
    auto* srv = server->get();

    // 1. Console / OP 4 执行 help, tps
    TestSender console{4, false, "Server"};
    CYANE_CHECK(cyane::game::CommandDispatcher::execute(console, *srv, "help"));
    CYANE_CHECK(!console.messages.empty());
    console.messages.clear();

    CYANE_CHECK(cyane::game::CommandDispatcher::execute(console, *srv, "/tps"));
    CYANE_CHECK_EQ(console.messages.size(), std::size_t{1});
    console.messages.clear();

    // 2. 时间命令：time set day, time add, time query
    CYANE_CHECK(cyane::game::CommandDispatcher::execute(console, *srv, "time set day"));
    CYANE_CHECK_EQ(srv->time_of_day(), 1000);
    CYANE_CHECK(cyane::game::CommandDispatcher::execute(console, *srv, "/time add 500"));
    CYANE_CHECK_EQ(srv->time_of_day(), 1500);

    // 3. 权限门控：非 OP 玩家执行管理员命令应被拒绝
    TestSender player{0, true, "Alice"};
    CYANE_CHECK(cyane::game::CommandDispatcher::execute(player, *srv, "/time set night"));
    CYANE_CHECK(!player.errors.empty()); // 报权限不足
    CYANE_CHECK_EQ(srv->time_of_day(), 1500); // 时间未被篡改

    // 4. Tab 补全：命令名
    const auto matches = cyane::game::CommandDispatcher::tab_complete(console, *srv, "/ti");
    CYANE_CHECK_EQ(matches.size(), std::size_t{1});
    CYANE_CHECK_EQ(matches[0], "/time");

    // 5. Tab 补全：/kill + 空格 → 补全实体选择器（而非重复 /kill）
    const auto kill_matches = cyane::game::CommandDispatcher::tab_complete(console, *srv, "/kill ");
    CYANE_CHECK(!kill_matches.empty());
    bool found_selector = false;
    for (const auto& m : kill_matches) {
        CYANE_CHECK(m != "/kill"); // 不应重复命令名
        if (m == "@a" || m == "@p" || m == "@r" || m == "@e" || m == "@s") {
            found_selector = true;
        }
    }
    CYANE_CHECK(found_selector);

    // 6. Tab 补全：/kill @ → 补全 @a @p @r @e @s
    const auto at_matches = cyane::game::CommandDispatcher::tab_complete(console, *srv, "/kill @");
    CYANE_CHECK_EQ(at_matches.size(), std::size_t{5});

    // 7. kill 实体选择器：@a 杀所有玩家（无玩家在线时不报错）
    CYANE_CHECK(cyane::game::CommandDispatcher::execute(console, *srv, "/kill @a"));

    // 8. kill @e 杀所有实体（无实体在线时不报错）
    CYANE_CHECK(cyane::game::CommandDispatcher::execute(console, *srv, "/kill @e"));

    // 9. Tab 补全：/weather + 空格 → 补全天气类型
    const auto weather_matches = cyane::game::CommandDispatcher::tab_complete(console, *srv, "/weather ");
    CYANE_CHECK_EQ(weather_matches.size(), std::size_t{3});
}
