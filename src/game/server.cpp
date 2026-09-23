#include "cyane/game/server.hpp"

#include <format>
#include <thread>

#include "cyane/core/log.hpp"
#include "cyane/generated/registry_meta.hpp"
#include "cyane/proto/json.hpp"
#include "cyane/proto/packet_ids.hpp"

namespace cyane {
namespace {

[[nodiscard]] Result<std::string> string_value(const Config& config, std::string_view key, std::string fallback) {
    if (!config.contains(key)) {
        return fallback;
    }
    auto parsed = config.get<std::string>(key);
    if (!parsed) {
        return make_error(ErrorCode::config, std::format("{}: expected a string", key));
    }
    return *parsed;
}

[[nodiscard]] Result<bool> bool_value(const Config& config, std::string_view key, bool fallback) {
    if (!config.contains(key)) {
        return fallback;
    }
    auto parsed = config.get<bool>(key);
    if (!parsed) {
        return make_error(ErrorCode::config, std::format("{}: expected a boolean", key));
    }
    return *parsed;
}

[[nodiscard]] Result<std::int64_t> int_value(
    const Config& config, std::string_view key, std::int64_t fallback, std::int64_t low, std::int64_t high) {
    std::int64_t value = fallback;
    if (config.contains(key)) {
        auto parsed = config.get<std::int64_t>(key);
        if (!parsed) {
            return make_error(ErrorCode::config, std::format("{}: expected an integer", key));
        }
        value = *parsed;
    }
    if (value < low || value > high) {
        return make_error(
            ErrorCode::config, std::format("{}: must be within [{}, {}], got {}", key, low, high, value));
    }
    return value;
}

}

Result<ServerConfig> ServerConfig::from(const Config& config) {
    ServerConfig out;

    auto bind = string_value(config, "network.bind", out.bind_address);
    if (!bind) {
        return std::unexpected{std::move(bind.error())};
    }
    out.bind_address = std::move(*bind);

    auto port = int_value(config, "network.port", out.port, 0, 65535);
    if (!port) {
        return std::unexpected{std::move(port.error())};
    }
    out.port = static_cast<std::uint16_t>(*port);

    auto motd = string_value(config, "network.motd", out.motd);
    if (!motd) {
        return std::unexpected{std::move(motd.error())};
    }
    out.motd = std::move(*motd);

    auto online_mode = bool_value(config, "network.online_mode", out.online_mode);
    if (!online_mode) {
        return std::unexpected{std::move(online_mode.error())};
    }
    out.online_mode = *online_mode;

    auto max_players = int_value(config, "server.max_players", out.max_players, 1, 100'000);
    if (!max_players) {
        return std::unexpected{std::move(max_players.error())};
    }
    out.max_players = static_cast<int>(*max_players);

    auto view_distance = int_value(config, "server.view_distance", out.view_distance, 3, 32);
    if (!view_distance) {
        return std::unexpected{std::move(view_distance.error())};
    }
    out.view_distance = static_cast<int>(*view_distance);

    auto tick_rate = int_value(config, "server.tick_rate", out.tick_rate, 1, 200);
    if (!tick_rate) {
        return std::unexpected{std::move(tick_rate.error())};
    }
    out.tick_rate = static_cast<int>(*tick_rate);

    auto workers = int_value(config, "server.worker_threads", 0, 0, 1024);
    if (!workers) {
        return std::unexpected{std::move(workers.error())};
    }
    out.worker_threads = static_cast<unsigned>(*workers);

    auto io_threads = int_value(config, "network.io_threads", 1, 1, 64);
    if (!io_threads) {
        return std::unexpected{std::move(io_threads.error())};
    }
    out.io_threads = static_cast<unsigned>(*io_threads);

    auto threshold =
        int_value(config, "network.compression_threshold", proto::kDefaultCompressionThreshold, -1, 65535);
    if (!threshold) {
        return std::unexpected{std::move(threshold.error())};
    }
    out.compression_threshold = static_cast<std::int32_t>(*threshold);

    auto world_dir = string_value(config, "server.world_dir", out.world_dir);
    if (!world_dir) {
        return std::unexpected{std::move(world_dir.error())};
    }
    out.world_dir = std::move(*world_dir);

    auto log_level = string_value(config, "log.level", out.log_level);
    if (!log_level) {
        return std::unexpected{std::move(log_level.error())};
    }
    if (!log::level_from_string(*log_level)) {
        return make_error(ErrorCode::config, std::format("log.level: unknown level '{}'", *log_level));
    }
    out.log_level = std::move(*log_level);

    auto log_file = string_value(config, "log.file", out.log_file);
    if (!log_file) {
        return std::unexpected{std::move(log_file.error())};
    }
    out.log_file = std::move(*log_file);

    return out;
}

Server::Server(ServerConfig config) : config_{std::move(config)} {}

Server::~Server() = default;

Result<std::unique_ptr<Server>> Server::create(ServerConfig config) {
    const unsigned hardware = std::thread::hardware_concurrency();
    if (config.worker_threads == 0) {
        config.worker_threads = hardware > 1 ? hardware - 1 : 1;
    }
    auto server = std::unique_ptr<Server>{new Server{std::move(config)}};
    server->workers_ = std::make_unique<ThreadPool>("chunk", server->config_.worker_threads);

    server->status_ = std::make_unique<game::ServerStatus>(
        server->config_.motd, static_cast<std::int32_t>(server->config_.max_players));

    net::ConnectionContext context;
    context.status = server->status_.get();
    context.online_mode = server->config_.online_mode;
    context.compression_threshold = server->config_.compression_threshold;
    context.disconnect_message = "CyaneServer: world system not implemented yet";
    server->player_manager_ = std::make_unique<entity::PlayerManager>();
    context.player_manager = server->player_manager_.get();
    server->hub_ = std::make_unique<net::PlayerHub>();
    context.hub = server->hub_.get();
    server->world_ = std::make_unique<world::World>();
    context.world = server->world_.get();
    context.view_distance = server->config_.view_distance;
    context.max_players = static_cast<std::int32_t>(server->config_.max_players);
    server->network_ = std::make_unique<net::NetService>(
        server->config_.bind_address, server->config_.port, server->config_.io_threads, std::move(context));
    return server;
}

int Server::run(std::uint64_t max_ticks) {
    log::info("cyaneserver {} | Minecraft {} (protocol {}) | tick {}Hz | view {} | {} workers | online-mode {}",
              kServerVersion,
              generated::kMinecraftVersion,
              generated::kProtocolVersion,
              config_.tick_rate,
              config_.view_distance,
              workers_->threads(),
              config_.online_mode);

    if (auto started = network_->start(); !started) {
        log::error("cannot listen on {}:{}: {}", config_.bind_address, config_.port, started.error().message);
        return 1;
    }

    Ticker ticker{config_.tick_rate};
    const auto rate = static_cast<std::uint64_t>(config_.tick_rate);

    while (running_.load(std::memory_order_relaxed)) {
        const auto tick_start = now();
        tick();
        stats_.record(elapsed_nanos(tick_start));
        ticker.wait_next();

        // 每秒结算一次 TPS 窗口，供 /tps 命令读取（不再打印刷屏日志）
        if (ticker.tick() % rate == 0) {
            stats_.complete_second();
        }
        if (max_ticks != 0 && ticker.tick() >= max_ticks) {
            break;
        }
    }

    const auto workers = workers_->stats();
    const auto accepted = network_->accepted();
    log::info("stopping after {} ticks ({} overruns) | worker tasks {} rejected {} | connections {}",
              ticker.tick(),
              ticker.overruns(),
              workers.executed,
              workers.rejected,
              accepted);
    network_->stop();
    workers_->shutdown();
    return 0;
}

void Server::tick() {
    status_->set_online(static_cast<std::int32_t>(network_->active()));
}

void Server::broadcast_system_message(std::string_view message) {
    // 以聊天框消息广播给所有在线玩家（position=0）
    ByteWriter chat;
    chat.string(proto::chat_text(message));
    chat.u8(0);
    hub_->broadcast_all(proto::play_cb::kChatMessage, chat.data());
}

}
