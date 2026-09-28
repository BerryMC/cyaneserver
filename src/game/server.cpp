#include "cyane/game/server.hpp"
#include "cyane/game/player_data.hpp"

#include <algorithm>
#include <format>
#include <iostream>
#include <thread>

#include "cyane/core/log.hpp"
#include "cyane/crypto/digest.hpp"
#include "cyane/generated/registry_meta.hpp"
#include "cyane/proto/json.hpp"
#include "cyane/proto/packet_ids.hpp"
#include "cyane/proto/play_fields.hpp"

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

    auto game_mode = string_value(config, "server.game_mode", out.game_mode);
    if (!game_mode) {
        return std::unexpected{std::move(game_mode.error())};
    }
    if (*game_mode != "survival" && *game_mode != "creative" && *game_mode != "spectator") {
        return make_error(ErrorCode::config, "server.game_mode must be 'survival', 'creative', or 'spectator'");
    }
    out.game_mode = std::move(*game_mode);

    auto op_file = string_value(config, "server.op_file", out.op_file);
    if (!op_file) {
        return std::unexpected{std::move(op_file.error())};
    }
    out.op_file = std::move(*op_file);

    auto recipe_file = string_value(config, "server.recipe_file", out.recipe_file);
    if (!recipe_file) {
        return std::unexpected{std::move(recipe_file.error())};
    }
    out.recipe_file = std::move(*recipe_file);

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

    auto player_data_dir = string_value(config, "server.player_data_dir", out.player_data_dir);
    if (!player_data_dir) {
        return std::unexpected{std::move(player_data_dir.error())};
    }
    out.player_data_dir = std::move(*player_data_dir);

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
    context.disconnect_message = "CyaneServer";
    server->op_manager_ = std::make_unique<game::OpManager>();
    if (const auto rc = server->op_manager_->load(server->config_.op_file); !rc) {
        log::warn("failed to load ops file: {}", rc.error().message);
    }
    server->crafting_ = std::make_unique<item::CraftingRegistry>();
    auto recipes = Config::load_file(server->config_.recipe_file);
    if (recipes) {
        if (const auto rc = server->crafting_->load_config(*recipes); !rc) {
            log::warn("failed to load recipes from {}: {}", server->config_.recipe_file, rc.error().message);
        } else {
            log::info("loaded {} recipes from {}", server->crafting_->size(), server->config_.recipe_file);
        }
    } else {
        log::warn("cannot open recipe file {}: {}", server->config_.recipe_file, recipes.error().message);
    }
    server->player_manager_ = std::make_unique<entity::PlayerManager>();
    context.player_manager = server->player_manager_.get();
    context.op_manager = server->op_manager_.get();
    context.crafting = server->crafting_.get();
    server->hub_ = std::make_unique<net::PlayerHub>();
    context.hub = server->hub_.get();
    server->item_drops_ = std::make_unique<net::ItemDropManager>();
    context.item_drops = server->item_drops_.get();
    server->containers_ = std::make_unique<net::ContainerStore>();
    context.containers = server->containers_.get();
    server->furnaces_ = std::make_unique<net::FurnaceStore>();
    context.furnaces = server->furnaces_.get();
    server->crafting_tables_ = std::make_unique<world::CraftingTableStore>();
    context.crafting_tables = server->crafting_tables_.get();
    server->mobs_ = std::make_unique<net::MobManager>();
    context.mobs = server->mobs_.get();
    // 玩家数据持久化存储
    server->player_data_store_ = std::make_unique<game::PlayerDataStore>();
    server->player_data_store_->set_dir(server->config_.player_data_dir);
    context.player_data_store = server->player_data_store_.get();
    if (recipes) {
        std::unordered_map<std::int16_t, std::int32_t> fuel;
        std::unordered_map<std::int16_t, std::pair<std::int16_t, std::uint8_t>> smelting;
        if (const auto rc = server->crafting_->load_furnace_config(*recipes, fuel, smelting); !rc) {
            log::warn("failed to load furnace tables: {}", rc.error().message);
        } else {
            net::FurnaceStore::SmeltMap smelt_entries;
            for (auto& [id, entry] : smelting) {
                smelt_entries[id] = net::FurnaceStore::SmeltEntry{entry.first, entry.second};
            }
            server->furnaces_->set_tables(std::move(fuel), std::move(smelt_entries));
        }
    }
    server->world_ = std::make_unique<world::World>();
    context.world = server->world_.get();
    context.tick_stats = &server->stats_;
    context.view_distance = server->config_.view_distance;
    context.max_players = static_cast<std::int32_t>(server->config_.max_players);
    context.game_mode = server->config_.game_mode == "survival" ? proto::game_mode::kSurvival
                         : (server->config_.game_mode == "spectator" ? proto::game_mode::kSpectator
                                                                      : proto::game_mode::kCreative);

    // 生成出生点附近的被动生物
    server->mobs_->spawn_passive(12);

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
    if (furnaces_ != nullptr) {
        furnaces_->tick();
        // 燃烧状态翻转 → 更新熔炉方块 meta 点亮位（bit 3）并广播，
        // 客户端方块材质才会切换到烧制中的样子
        const std::int32_t radius = std::clamp(config_.view_distance, 2, 8);
        for (const auto& [key, lit] : furnaces_->take_lit_changes()) {
            const std::int32_t bx = position_x(key);
            const std::int32_t by = position_y(key);
            const std::int32_t bz = position_z(key);
            const std::uint16_t cur = world_->block_at(bx, by, bz);
            if (world::block_id(cur) != world::block_id(world::kStateFurnace)) {
                continue;
            }
            auto meta = world::state_meta(cur);
            meta = static_cast<std::uint16_t>((meta & ~std::uint16_t(8)) | (lit ? 8 : 0));
            const std::uint16_t next = static_cast<std::uint16_t>(world::block_id(cur) << 4) | meta;
            if (next == cur) {
                continue;
            }
            world_->set_block(bx, by, bz, next);
            ByteWriter change;
            change.position(bx, by, bz);
            change.varint(static_cast<std::int32_t>(next));
            const auto cpos = world::ChunkPos::from_world(bx, bz);
            if (cpos) {
                hub_->broadcast_near(cpos->x, cpos->z, radius, 0, proto::play_cb::kBlockChange,
                                      change.data());
            }
        }
    }
    if (mobs_ != nullptr) {
        const std::int32_t radius = std::clamp(config_.view_distance, 2, 8);
        for (const auto& mob : mobs_->tick()) {
            // 只发给生物所在区块视距内的玩家（远端客户端看不到该实体）
            const auto cpos = world::ChunkPos::from_world(static_cast<std::int32_t>(mob.pos.x),
                                                           static_cast<std::int32_t>(mob.pos.z));
            if (!cpos) {
                continue;
            }
            // EntityTeleport (0x4C) + EntityHeadLook (0x36)
            ByteWriter tp;
            tp.varint(static_cast<std::int32_t>(mob.entity_id));
            tp.f64(mob.pos.x);
            tp.f64(mob.pos.y);
            tp.f64(mob.pos.z);
            tp.u8(net::angle_byte(mob.pos.yaw));
            tp.u8(0);
            tp.boolean(true);
            hub_->broadcast_near(cpos->x, cpos->z, radius, 0, proto::play_cb::kEntityTeleport,
                                 tp.data());
            ByteWriter head;
            head.varint(static_cast<std::int32_t>(mob.entity_id));
            head.u8(net::angle_byte(mob.pos.yaw));
            hub_->broadcast_near(cpos->x, cpos->z, radius, 0, proto::play_cb::kEntityHeadLook,
                                 head.data());
        }
    }
}

void Server::broadcast_system_message(std::string_view message) {
    // 以聊天框消息广播给所有在线玩家（position=0）
    ByteWriter chat;
    chat.string(proto::chat_text(message));
    chat.u8(0);
    hub_->broadcast_all(proto::play_cb::kChatMessage, chat.data());
}

bool Server::kill_player_by_name(std::string_view name) {
    const std::uint32_t target_id = hub_->player_id_by_name(name);
    if (target_id == 0) {
        return false;
    }
    return hub_->send_kill(target_id);
}

bool Server::set_player_gamemode(std::string_view name, std::string_view mode) {
    const std::uint32_t target_id = hub_->player_id_by_name(name);
    if (target_id == 0) {
        return false;
    }
    std::uint8_t gm;
    if (mode == "survival") gm = proto::game_mode::kSurvival;
    else if (mode == "creative") gm = proto::game_mode::kCreative;
    else if (mode == "adventure") gm = proto::game_mode::kAdventure;
    else if (mode == "spectator") gm = proto::game_mode::kSpectator;
    else return false;
    // 1.12.2 用 PlayerInfo(0x2E) action=2 (CHANGE_GAME_MODE 序数) 更新游戏模式；
    // 同时把切换指令投递给目标连接，使其行为判定与 PlayerAbilities 一并更新
    ByteWriter info;
    info.varint(proto::play_cb::kPlayerInfoUpdateGameType);
    info.varint(1);
    const auto uuid_opt = hub_->player_uuid_by_name(name);
    if (!uuid_opt) return false;
    info.bytes(ByteSpan{reinterpret_cast<const std::byte*>(uuid_opt->data()), uuid_opt->size()});
    info.varint(static_cast<std::int32_t>(gm));
    hub_->broadcast_all(proto::play_cb::kPlayerInfo, info.data());
    return hub_->send_gamemode(target_id, gm);
}

bool Server::op_player(std::string_view name) {
    const auto uuid_opt = hub_->player_uuid_by_name(name);
    if (!uuid_opt) {
        return false;
    }
    const std::string uuid_str = Uuid::from_bytes(*uuid_opt).dashed();
    return op_manager_->op_player(uuid_str, name);
}

bool Server::deop_player(std::string_view name) {
    const auto uuid_opt = hub_->player_uuid_by_name(name);
    if (!uuid_opt) {
        return false;
    }
    const std::string uuid_str = Uuid::from_bytes(*uuid_opt).dashed();
    return op_manager_->deop_player(uuid_str);
}

std::vector<std::string> Server::player_names() const {
    return hub_->all_player_names();
}

}
