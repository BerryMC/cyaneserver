#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <utility>

#include "cyane/core/config.hpp"
#include "cyane/game/command.hpp"
#include "cyane/game/op_manager.hpp"
#include "cyane/game/default_config.hpp"
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
    console.messages.clear();

    // 原版 CommandTime：query daytime 返回 worldTime % 24000
    CYANE_CHECK(cyane::game::CommandDispatcher::execute(console, *srv, "time set 25000"));
    CYANE_CHECK_EQ(srv->time_of_day(), 25000);  // setWorldTime 存原始值
    console.messages.clear();
    CYANE_CHECK(cyane::game::CommandDispatcher::execute(console, *srv, "time query daytime"));
    CYANE_CHECK_EQ(console.messages.at(0), std::string{"Time is 1000"});
    console.messages.clear();
    CYANE_CHECK(cyane::game::CommandDispatcher::execute(console, *srv, "time query day"));
    CYANE_CHECK_EQ(console.messages.at(0), std::string{"Time is 1"});
    console.messages.clear();
    // 1.12.2 没有 noon / midnight 关键字
    CYANE_CHECK(cyane::game::CommandDispatcher::execute(console, *srv, "time set noon"));
    CYANE_CHECK_EQ(console.messages.at(0), std::string{"Unknown time: 'noon'"});
    console.messages.clear();
    CYANE_CHECK(cyane::game::CommandDispatcher::execute(console, *srv, "time set day"));

    // 3. 权限门控：非 OP 玩家执行管理员命令应被拒绝
    TestSender player{0, true, "Alice"};
    CYANE_CHECK(cyane::game::CommandDispatcher::execute(player, *srv, "/time set night"));
    CYANE_CHECK(!player.errors.empty()); // 报权限不足
    CYANE_CHECK_EQ(srv->time_of_day(), 1000); // 时间未被篡改（上一条已设回 day）

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

CYANE_TEST(op_manager_loads_vanilla_array_and_legacy_map) {
    const auto path = std::filesystem::temp_directory_path() / "cyane_ops_test.json";

    // 1) 原版数组格式（含 bypassesPlayerLimit 布尔字段）
    {
        std::ofstream out{path};
        out << R"([{"uuid":"75bcdcde-9c24-35db-ba26-4913e8151672","name":"CmdTester",)"
               R"("level":4,"bypassesPlayerLimit":false}])";
    }
    cyane::game::OpManager ops;
    CYANE_CHECK(ops.load(path.string()).has_value());
    CYANE_CHECK_EQ(static_cast<int>(ops.op_level("75bcdcde-9c24-35db-ba26-4913e8151672")), 4);
    // 大小写不敏感
    CYANE_CHECK_EQ(static_cast<int>(ops.op_level("75BCDCDE-9C24-35DB-BA26-4913E8151672")), 4);
    CYANE_CHECK_EQ(static_cast<int>(ops.op_level("00000000-0000-0000-0000-000000000000")), 0);

    // 2) 旧版映射格式（名字/等级在前，后面跟布尔字段）
    {
        std::ofstream out{path};
        out << R"({"75bcdcde-9c24-35db-ba26-4913e8151672":{"name":"LegacyOp","level":2,)"
               R"("bypassesPlayerLimit":false}})";
    }
    cyane::game::OpManager legacy;
    CYANE_CHECK(legacy.load(path.string()).has_value());
    CYANE_CHECK_EQ(static_cast<int>(legacy.op_level("75bcdcde-9c24-35db-ba26-4913e8151672")), 2);

    // 3) save() 写回原版数组格式，且保留名称原始大小写
    legacy.save();
    const auto saved = [&] {
        std::ifstream in{path};
        std::stringstream buffer;
        buffer << in.rdbuf();
        return buffer.str();
    }();
    CYANE_CHECK(saved.find("\"uuid\"") != std::string::npos);
    CYANE_CHECK(saved.find("LegacyOp") != std::string::npos);
    CYANE_CHECK(saved.find("\"level\": 2") != std::string::npos);

    // 4) 重新加载自己写出的文件仍可用
    cyane::game::OpManager reopened;
    CYANE_CHECK(reopened.load(path.string()).has_value());
    CYANE_CHECK_EQ(static_cast<int>(reopened.op_level("75bcdcde-9c24-35db-ba26-4913e8151672")), 2);

    // 5) 缺文件时创建空数组而不是报错
    const auto missing = std::filesystem::temp_directory_path() / "cyane_ops_missing.json";
    std::filesystem::remove(missing);
    cyane::game::OpManager fresh;
    CYANE_CHECK(fresh.load(missing.string()).has_value());
    CYANE_CHECK_EQ(static_cast<int>(fresh.op_level("75bcdcde-9c24-35db-ba26-4913e8151672")), 0);
    std::filesystem::remove(missing);
    std::filesystem::remove(path);
}

CYANE_TEST(command_permission_levels_match_vanilla) {
    auto settings = fast_config();
    CYANE_CHECK(settings.has_value());
    if (!settings) return;
    auto server = cyane::Server::create(std::move(*settings));
    CYANE_CHECK(server.has_value());
    if (!server) return;
    auto* srv = server->get();

    // CommandBroadcast = 1：0 级被拒，1 级可发言
    TestSender zero{0, true, "Zero"};
    CYANE_CHECK(cyane::game::CommandDispatcher::execute(zero, *srv, "say hello"));
    CYANE_CHECK(!zero.errors.empty());
    TestSender one{1, true, "One"};
    CYANE_CHECK(cyane::game::CommandDispatcher::execute(one, *srv, "say hello"));
    CYANE_CHECK(one.errors.empty());

    // CommandKill = 2：0 级即使无参也不能自杀
    TestSender noop{0, true, "Noop"};
    CYANE_CHECK(cyane::game::CommandDispatcher::execute(noop, *srv, "kill"));
    CYANE_CHECK(!noop.errors.empty());

    // CommandOp / CommandDeOp = 3：3 级足够，2 级不足
    TestSender level2{2, true, "Two"};
    CYANE_CHECK(cyane::game::CommandDispatcher::execute(level2, *srv, "op Nobody"));
    CYANE_CHECK(!level2.errors.empty());
    TestSender level3{3, true, "Three"};
    CYANE_CHECK(cyane::game::CommandDispatcher::execute(level3, *srv, "deop Nobody"));
    // 3 级通过权限检查（具体结果取决于玩家是否存在，不能是权限错误）
    bool permission_denied = false;
    for (const auto& message : level3.errors) {
        if (message.find("permission") != std::string::npos) permission_denied = true;
    }
    CYANE_CHECK(!permission_denied);
}

CYANE_TEST(gamemode_accepts_vanilla_short_names_and_ids) {
    auto settings = fast_config();
    CYANE_CHECK(settings.has_value());
    if (!settings) return;
    auto server = cyane::Server::create(std::move(*settings));
    CYANE_CHECK(server.has_value());
    if (!server) return;
    auto* srv = server->get();

    // GameType.byName：全名与 shortName 都要被接受（此前执行层只认全名，简写静默失败）
    for (const auto name : {"survival", "s", "creative", "c", "adventure", "a", "spectator", "sp"}) {
        const auto mode = cyane::proto::game_mode::parse(name);
        CYANE_CHECK(mode.has_value());
    }
    CYANE_CHECK(!cyane::proto::game_mode::parse("nonsense").has_value());
    CYANE_CHECK_EQ(static_cast<int>(*cyane::proto::game_mode::parse("sp")), 3);
    CYANE_CHECK_EQ(static_cast<int>(*cyane::proto::game_mode::from_id(2)), 2);
    CYANE_CHECK(!cyane::proto::game_mode::from_id(4).has_value());

    // 命令层：简写不再报 "Unknown game mode"
    TestSender console{4, false, "Server"};
    for (const auto name : {"c", "s", "sp", "a", "creative"}) {
        console.messages.clear();
        console.errors.clear();
        CYANE_CHECK(cyane::game::CommandDispatcher::execute(
            console, *srv, std::string{"gamemode "} + name));
        for (const auto& message : console.errors) {
            CYANE_CHECK(message.find("Unknown game mode") == std::string::npos);
        }
    }
}

CYANE_TEST(weather_command_drives_rain_and_thunder_state) {
    auto settings = fast_config();
    CYANE_CHECK(settings.has_value());
    auto server = cyane::Server::create(std::move(*settings));
    CYANE_CHECK(server.has_value());
    auto* srv = server->get();
    TestSender console{4, false, "Server"};

    // 初始晴天
    CYANE_CHECK(!srv->is_raining());
    CYANE_CHECK(!srv->is_thundering());

    // /weather rain 600：rain_time/thunder_time = 600*20，只有雨没有雷
    CYANE_CHECK(cyane::game::CommandDispatcher::execute(console, *srv, "weather rain 600"));
    CYANE_CHECK(srv->is_raining());
    CYANE_CHECK(!srv->is_thundering());

    // /weather thunder 100
    console.messages.clear();
    CYANE_CHECK(cyane::game::CommandDispatcher::execute(console, *srv, "weather THUNDER 100"));
    CYANE_CHECK(srv->is_raining());
    CYANE_CHECK(srv->is_thundering());

    // /weather clear：清天气并把 rain/thunder 时间归零
    console.messages.clear();
    CYANE_CHECK(cyane::game::CommandDispatcher::execute(console, *srv, "weather clear"));
    CYANE_CHECK(!srv->is_raining());
    CYANE_CHECK(!srv->is_thundering());
    CYANE_CHECK_EQ(console.messages.size(), std::size_t{1});

    // 时长越界被拒（parseInt(min=1, max=1000000)）
    console.messages.clear();
    CYANE_CHECK(cyane::game::CommandDispatcher::execute(console, *srv, "weather rain 0"));
    CYANE_CHECK(!srv->is_raining());
    CYANE_CHECK_EQ(console.messages.size(), std::size_t{1});
    CYANE_CHECK(console.messages[0].find("less than 1") != std::string::npos);
}

CYANE_TEST(difficulty_command_sets_state_and_names) {
    auto settings = fast_config();
    CYANE_CHECK(settings.has_value());
    auto server = cyane::Server::create(std::move(*settings));
    CYANE_CHECK(server.has_value());
    auto* srv = server->get();
    TestSender console{4, false, "Server"};

    CYANE_CHECK(cyane::game::CommandDispatcher::execute(console, *srv, "difficulty hard"));
    CYANE_CHECK(srv->difficulty() == cyane::world::EnumDifficulty::Hard);
    CYANE_CHECK(console.messages.size() == 1);
    CYANE_CHECK(console.messages[0] == "The difficulty has been set to Hard");

    // 首字母简写 + 大小写不敏感
    console.messages.clear();
    CYANE_CHECK(cyane::game::CommandDispatcher::execute(console, *srv, "difficulty P"));
    CYANE_CHECK(srv->difficulty() == cyane::world::EnumDifficulty::Peaceful);
    CYANE_CHECK(console.messages[0] == "The difficulty has been set to Peaceful");

    // 非法值：commands.difficulty.failure
    console.messages.clear();
    CYANE_CHECK(cyane::game::CommandDispatcher::execute(console, *srv, "difficulty nope"));
    CYANE_CHECK(console.messages.size() == 1);
    CYANE_CHECK(console.messages[0].find("Difficulty does not exist") != std::string::npos);
    CYANE_CHECK(srv->difficulty() == cyane::world::EnumDifficulty::Peaceful);
}

CYANE_TEST(gamerule_command_matches_vanilla_semantics) {
    auto settings = fast_config();
    CYANE_CHECK(settings.has_value());
    auto server = cyane::Server::create(std::move(*settings));
    CYANE_CHECK(server.has_value());
    auto* srv = server->get();
    TestSender console{4, false, "Server"};

    // 无参：列出全部规则名（20+ 条，逗号分隔）
    CYANE_CHECK(cyane::game::CommandDispatcher::execute(console, *srv, "gamerule"));
    CYANE_CHECK_EQ(console.messages.size(), std::size_t{1});
    CYANE_CHECK(console.messages[0].find("keepInventory") != std::string::npos);
    CYANE_CHECK(console.messages[0].find(", ") != std::string::npos);

    // 单参：查询 "name = value"
    console.messages.clear();
    CYANE_CHECK(cyane::game::CommandDispatcher::execute(console, *srv, "gamerule keepInventory"));
    CYANE_CHECK_EQ(console.messages.size(), std::size_t{1});
    CYANE_CHECK_EQ(console.messages[0], std::string("keepInventory = false"));

    // 双参：设置成功
    console.messages.clear();
    CYANE_CHECK(cyane::game::CommandDispatcher::execute(console, *srv, "gamerule keepInventory true"));
    CYANE_CHECK(srv->game_rules().get_bool("keepInventory"));
    CYANE_CHECK_EQ(console.messages[0], std::string("Game rule keepInventory has been updated to true"));

    // 布尔规则给非 true/false：commands.generic.boolean.invalid
    CYANE_CHECK(srv->game_rules().is_boolean("keepInventory"));
    console.messages.clear();
    CYANE_CHECK(cyane::game::CommandDispatcher::execute(console, *srv, "gamerule keepInventory yes"));
    CYANE_CHECK(srv->game_rules().get_bool("keepInventory"));
    CYANE_CHECK(console.messages[0].find("not a valid boolean") != std::string::npos);

    // 未知规则查询
    console.messages.clear();
    CYANE_CHECK(cyane::game::CommandDispatcher::execute(console, *srv, "gamerule noSuchRule"));
    CYANE_CHECK(console.messages[0].find("There is no game rule named noSuchRule") != std::string::npos);
}

CYANE_TEST(config_load_or_create_writes_default_and_preserves_existing) {
    const auto dir = std::filesystem::temp_directory_path() / "cyane_config_or_create";
    std::filesystem::remove_all(dir);
    const auto path = dir / "nested" / "server.toml";

    // 文件不存在：连父目录一起创建，并写入默认内容
    const auto created = cyane::Config::load_or_create(path, "[network]\nport = 1234\n");
    CYANE_CHECK(created.has_value());
    CYANE_CHECK(std::filesystem::exists(path));
    CYANE_CHECK_EQ(created->get<std::int64_t>("network.port").value_or(0), 1234);

    // 已存在：绝不覆盖用户配置
    const auto again = cyane::Config::load_or_create(path, "[network]\nport = 9999\n");
    CYANE_CHECK(again.has_value());
    CYANE_CHECK_EQ(again->get<std::int64_t>("network.port").value_or(0), 1234);

    std::filesystem::remove_all(dir);
}

CYANE_TEST(default_server_toml_parses_and_matches_server_config_defaults) {
    const auto config = cyane::Config::parse(cyane::kDefaultServerToml);
    CYANE_CHECK(config.has_value());
    const auto server = cyane::ServerConfig::from(*config);
    CYANE_CHECK(server.has_value());
    // 内置默认配置必须能被解析成一份完整可用的 ServerConfig
    CYANE_CHECK_EQ(static_cast<int>(server->port), 25565);
    CYANE_CHECK_EQ(server->max_players, 20);
    CYANE_CHECK_EQ(server->view_distance, 8);
    CYANE_CHECK_EQ(server->world_dir, std::string("world"));
    CYANE_CHECK_EQ(server->op_file, std::string("config/ops.json"));
}
