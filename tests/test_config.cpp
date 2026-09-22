#include <cstdint>
#include <string>
#include <string_view>

#include "cyane/core/config.hpp"
#include "cyane/game/server.hpp"
#include "test_framework.hpp"

namespace {

template <typename T>
T must_get(const cyane::Config& config, std::string_view key) {
    const auto value = config.get<T>(key);
    CYANE_CHECK(value.has_value());
    return value.value_or(T{});
}

}

CYANE_TEST(config_parses_sections_and_scalars) {
    const auto config = cyane::Config::parse(R"(
# leading comment
[network]
bind = "0.0.0.0"   # trailing comment
port = 25565
online_mode = true

[server]
view_distance = 10
motd = 'literal # kept'
ratio = 1.5
)");
    CYANE_CHECK(config.has_value());
    CYANE_CHECK_EQ(must_get<std::string>(*config, "network.bind"), std::string{"0.0.0.0"});
    CYANE_CHECK_EQ(must_get<std::int64_t>(*config, "network.port"), std::int64_t{25565});
    CYANE_CHECK_EQ(must_get<bool>(*config, "network.online_mode"), true);
    CYANE_CHECK_EQ(must_get<std::int64_t>(*config, "server.view_distance"), std::int64_t{10});
    CYANE_CHECK_EQ(must_get<std::string>(*config, "server.motd"), std::string{"literal # kept"});
    CYANE_CHECK_NEAR(must_get<double>(*config, "server.ratio"), 1.5, 1e-9);
}

CYANE_TEST(config_handles_escapes_and_arrays) {
    const auto config = cyane::Config::parse(R"(
[list]
names = ["alpha", 'beta', gamma]
quoted = "tab\there"
empty = []
)");
    CYANE_CHECK(config.has_value());
    const auto names = must_get<cyane::Config::Array>(*config, "list.names");
    CYANE_CHECK_EQ(names.size(), std::size_t{3});
    CYANE_CHECK_EQ(names[0], std::string{"alpha"});
    CYANE_CHECK_EQ(names[1], std::string{"beta"});
    CYANE_CHECK_EQ(names[2], std::string{"gamma"});
    CYANE_CHECK_EQ(must_get<std::string>(*config, "list.quoted"), std::string{"tab\there"});
    CYANE_CHECK_EQ(must_get<cyane::Config::Array>(*config, "list.empty").size(), std::size_t{0});
}

CYANE_TEST(config_keeps_dotted_keys_flat) {
    const auto config = cyane::Config::parse("plain = 3\n");
    CYANE_CHECK(config.has_value());
    CYANE_CHECK_EQ(must_get<std::int64_t>(*config, "plain"), std::int64_t{3});
    CYANE_CHECK(!config->contains("missing"));
}

CYANE_TEST(config_rejects_type_mismatch_without_throwing) {
    const auto config = cyane::Config::parse("[network]\nport = \"25565\"\n");
    CYANE_CHECK(config.has_value());
    CYANE_CHECK(!config->get<std::int64_t>("network.port").has_value());
    CYANE_CHECK_EQ(config->get_or<std::int64_t>("network.port", 42), std::int64_t{42});
}

CYANE_TEST(config_reports_syntax_errors_with_location) {
    const auto missing_equals = cyane::Config::parse("broken\n", "test.toml");
    CYANE_CHECK(!missing_equals.has_value());
    CYANE_CHECK(missing_equals.error().code == cyane::ErrorCode::config);
    CYANE_CHECK(missing_equals.error().message.find("test.toml:1") != std::string::npos);

    CYANE_CHECK(!cyane::Config::parse("key = \"unterminated\n").has_value());
    CYANE_CHECK(!cyane::Config::parse("key = [1, 2\n").has_value());
    CYANE_CHECK(!cyane::Config::parse("key = nope\n").has_value());
    CYANE_CHECK(!cyane::Config::parse("key = \"bad\\q\"\n").has_value());
    CYANE_CHECK(!cyane::Config::parse("key =\n").has_value());
    CYANE_CHECK(!cyane::Config::parse("[unclosed\n").has_value());
}

CYANE_TEST(config_load_file_reports_missing_file) {
    const auto config = cyane::Config::load_file("/nonexistent/cyane-server.toml");
    CYANE_CHECK(!config.has_value());
    CYANE_CHECK(config.error().code == cyane::ErrorCode::config);
}

CYANE_TEST(server_config_applies_defaults) {
    const auto config = cyane::Config::parse("");
    CYANE_CHECK(config.has_value());
    const auto server = cyane::ServerConfig::from(*config);
    CYANE_CHECK(server.has_value());
    CYANE_CHECK_EQ(server->port, std::uint16_t{25565});
    CYANE_CHECK_EQ(server->tick_rate, 20);
    CYANE_CHECK_EQ(server->view_distance, 10);
    CYANE_CHECK(server->online_mode);
}

CYANE_TEST(server_config_rejects_invalid_values) {
    const auto port = cyane::Config::parse("[network]\nport = 70000\n");
    CYANE_CHECK(port.has_value());
    const auto bad_port = cyane::ServerConfig::from(*port);
    CYANE_CHECK(!bad_port.has_value());
    CYANE_CHECK(bad_port.error().code == cyane::ErrorCode::config);

    const auto view = cyane::Config::parse("[server]\nview_distance = 99\n");
    CYANE_CHECK(view.has_value());
    CYANE_CHECK(!cyane::ServerConfig::from(*view).has_value());

    const auto level = cyane::Config::parse("[log]\nlevel = \"loud\"\n");
    CYANE_CHECK(level.has_value());
    CYANE_CHECK(!cyane::ServerConfig::from(*level).has_value());

    const auto typed = cyane::Config::parse("[network]\nport = true\n");
    CYANE_CHECK(typed.has_value());
    CYANE_CHECK(!cyane::ServerConfig::from(*typed).has_value());
}
