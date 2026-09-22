#include <chrono>
#include <cstdint>
#include <thread>
#include <utility>

#include "cyane/core/config.hpp"
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
