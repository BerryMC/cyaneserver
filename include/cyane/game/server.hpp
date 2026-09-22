#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "cyane/core/config.hpp"
#include "cyane/core/error.hpp"
#include "cyane/core/thread_pool.hpp"
#include "cyane/core/time.hpp"
#include "cyane/entity/player_manager.hpp"
#include "cyane/game/status.hpp"
#include "cyane/net/net_service.hpp"
#include "cyane/world/world.hpp"

namespace cyane {

inline constexpr std::string_view kServerVersion = "0.1.0";

struct ServerConfig {
    std::string bind_address{"0.0.0.0"};
    std::uint16_t port{25565};
    std::string motd{"CyaneServer 1.12.2"};
    int max_players{20};
    int view_distance{10};
    bool online_mode{true};
    int tick_rate{20};
    unsigned worker_threads{0};
    unsigned io_threads{1};
    std::int32_t compression_threshold{proto::kDefaultCompressionThreshold};
    std::string world_dir{"world"};
    std::string log_level{"info"};
    std::string log_file{"logs/latest.log"};

    [[nodiscard]] static Result<ServerConfig> from(const Config& config);
};

class Server {
public:
    [[nodiscard]] static Result<std::unique_ptr<Server>> create(ServerConfig config);
    ~Server();

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    int run(std::uint64_t max_ticks = 0);
    void request_stop() noexcept { running_.store(false, std::memory_order_relaxed); }

    [[nodiscard]] const ServerConfig& config() const noexcept { return config_; }
    [[nodiscard]] const TickStats& stats() const noexcept { return stats_; }
    [[nodiscard]] const net::NetService& network() const noexcept { return *network_; }
    [[nodiscard]] const game::ServerStatus& status() const noexcept { return *status_; }

private:
    explicit Server(ServerConfig config);

    void tick();
    void report_status();

    ServerConfig config_;
    std::unique_ptr<game::ServerStatus> status_;
    std::unique_ptr<entity::PlayerManager> player_manager_;
    std::unique_ptr<world::World> world_;
    std::unique_ptr<net::NetService> network_;
    std::unique_ptr<ThreadPool> workers_;
    TickStats stats_;
    std::atomic<bool> running_{true};
};

}
