#include "cyane/game/server.hpp"
#include "cyane/game/player_data.hpp"

#include <algorithm>
#include <format>
#include <iostream>
#include <random>
#include <thread>
#include <unordered_set>

#include "cyane/core/log.hpp"
#include "cyane/crypto/digest.hpp"
#include "cyane/generated/registry_meta.hpp"
#include "cyane/net/packet_writers.hpp"
#include "cyane/proto/json.hpp"
#include "cyane/proto/packet_ids.hpp"
#include "cyane/proto/play_fields.hpp"
#include "cyane/item/item_tools.hpp"
#include "cyane/world/block_drops.hpp"
#include "cyane/world/level_dat.hpp"

namespace cyane {

// 骷髅箭的 SpawnObject object id（R-022：EntityTrackerEntry 对 EntityArrow 传 60）
inline constexpr std::uint8_t kObjectTypeArrow = 60;
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

    auto autosave = int_value(config, "server.autosave_interval", out.autosave_interval, 0, 86'400);
    if (!autosave) {
        return std::unexpected{std::move(autosave.error())};
    }
    out.autosave_interval = static_cast<int>(*autosave);

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
    server->block_ticks_ = std::make_unique<net::BlockTicks>();
    context.block_ticks = server->block_ticks_.get();
    server->projectiles_ = std::make_unique<net::ProjectileManager>();
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
    server->xp_orb_manager_ = std::make_unique<net::XPOrbManager>();
    context.xp_orbs = server->xp_orb_manager_.get();
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
    context.save_world = [srv = server.get()] { srv->request_save(); };
    context.max_players = static_cast<std::int32_t>(server->config_.max_players);
    context.game_mode = server->config_.game_mode == "survival" ? proto::game_mode::kSurvival
                         : (server->config_.game_mode == "spectator" ? proto::game_mode::kSpectator
                                                                      : proto::game_mode::kCreative);

    // 世界元数据：level.dat 出生点（首启缺失时建档最小集）
    if (auto created = world::ensure_level_dat(server->config_.world_dir,
                                                static_cast<std::int32_t>(context.game_mode));
        !created) {
        log::warn("cannot ensure level.dat: {}", created.error().message);
    } else if (*created) {
        log::info("created {} with minimal level.dat", server->config_.world_dir);
    }
    if (auto level = world::load_level_dat(server->config_.world_dir); !level) {
        log::warn("cannot read level.dat: {}", level.error().message);
    } else {
        context.spawn_x = level->spawn_x;
        context.spawn_y = level->spawn_y;
        context.spawn_z = level->spawn_z;
        log::info("world spawn at ({}, {}, {})", level->spawn_x, level->spawn_y, level->spawn_z);
    }

    // 世界存档：载入 region/*.mca（方块编辑 + 方块实体 + 掉落物/生物实体），
    // 并注入按需加载（被视距释放的区块回归时从磁盘重读真实地形）
    server->persistence_ = std::make_unique<game::WorldPersistence>(
        *server->world_, *server->containers_, *server->furnaces_, *server->item_drops_,
        *server->mobs_, server->config_.world_dir);
    server->persistence_->attach_loader();
    if (auto loaded = server->persistence_->load(); !loaded) {
        log::warn("world load failed: {}", loaded.error().message);
    } else if (*loaded > 0) {
        log::info("loaded {} chunks from {}", *loaded, server->config_.world_dir);
    }

    // 生成出生点附近的被动生物；存档已有生物时不再重复生成
    if (server->mobs_->size() == 0) {
        server->mobs_->spawn_passive(12, *server->world_,
                                     static_cast<double>(context.spawn_x) + 0.5,
                                     static_cast<double>(context.spawn_z) + 0.5);
    }

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
    // 停机前落盘世界（方块编辑 + 箱子/熔炉）
    save_world_now();
    workers_->shutdown();
    return 0;
}

void Server::save_world_now() {
    if (persistence_ == nullptr) {
        return;
    }
    if (auto saved = persistence_->save(); !saved) {
        log::warn("world save failed: {}", saved.error().message);
    } else {
        log::info("saved {} chunks to {}", *saved, config_.world_dir);
    }
}

void Server::tick() {
    // 延迟方块更新（按钮回弹）：到期清位并广播，与按下者是否在线无关
    if (block_ticks_ != nullptr && world_ != nullptr && hub_ != nullptr) {
        block_ticks_->tick(now_ms(), *world_, *hub_, config_.view_distance);
    }
    // 在线数 = 已进入 play 阶段的玩家（hub 注册表），而非活跃 TCP 连接——
    // 后者会把 server list ping 的握手连接也计成玩家
    status_->set_online(hub_ != nullptr ? static_cast<std::int32_t>(hub_->size()) : 0);
    // 周期性世界存档（autosave_interval 秒，0 = 关闭）+ 断开触发的防抖保存
    const bool autosave_due = config_.autosave_interval > 0 &&
        ++ticks_since_save_ >= static_cast<std::uint64_t>(config_.autosave_interval) *
                               static_cast<std::uint64_t>(config_.tick_rate);
    const bool disconnect_save = save_pending_.exchange(false, std::memory_order_relaxed);
    if (persistence_ != nullptr && (autosave_due || disconnect_save) &&
        persistence_->needs_save()) {
        const auto now = std::chrono::steady_clock::now();
        const auto since_last = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    now - last_save_time_).count();
        if (disconnect_save && !autosave_due &&
            static_cast<std::uint64_t>(since_last) < kMinSaveIntervalMs) {
            // 距上次保存不足最小间隔：推迟到下一个 autosave 窗口（或下次触发）
            save_pending_.store(true, std::memory_order_relaxed);
        } else {
            ticks_since_save_ = 0;
            last_save_time_ = now;
            if (auto saved = persistence_->save(); !saved) {
                log::warn("world save failed: {}", saved.error().message);
            } else if (*saved > 0) {
                log::info("saved {} chunks to {}", *saved, config_.world_dir);
            }
        }
    }
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
    if (item_drops_ != nullptr && world_ != nullptr) {
        apply_item_tick(item_drops_->tick(*world_));
    }
    // 更新经验球
    if (xp_orb_manager_ != nullptr && world_ != nullptr && hub_ != nullptr) {
        const std::int32_t radius = std::clamp(config_.view_distance, 2, 8);
        const auto players = hub_->others(net::kNoExclude);
        const auto xp_events = xp_orb_manager_->tick(*world_, players);
        // 出生包
        for (const auto& spawned : xp_events.spawned) {
            ByteWriter spawn;
            net::writers::write_spawn_experience_orb(spawn, spawned.entity_id, spawned.x,
                                                     spawned.y, spawned.z, spawned.value);
            const auto cpos = world::ChunkPos::from_world(static_cast<std::int32_t>(spawned.x),
                                                          static_cast<std::int32_t>(spawned.z));
            if (cpos) {
                hub_->broadcast_near(cpos->x, cpos->z, radius, 0, proto::play_cb::kSpawnExperienceOrb,
                                     spawn.data());
            }
        }
        // 位置同步
        for (const auto& tp : xp_events.teleported) {
            ByteWriter teleport;
            net::writers::write_entity_teleport(teleport, tp.entity_id, tp.x, tp.y, tp.z, 0.0f, 0.0f,
                                                true);
            const auto cpos = world::ChunkPos::from_world(static_cast<std::int32_t>(tp.x),
                                                          static_cast<std::int32_t>(tp.z));
            if (cpos) {
                hub_->broadcast_near(cpos->x, cpos->z, radius, 0, proto::play_cb::kEntityTeleport,
                                     teleport.data());
            }
        }
        // 销毁（消失/拾取）
        for (const auto& destroyed : xp_events.destroyed) {
            ByteWriter destroy;
            const std::uint32_t ids[] = {destroyed.entity_id};
            net::writers::write_destroy_entities(destroy, ids);
            // 广播给附近玩家
            // （这里我们没有位置信息，所以广播给所有玩家）
            hub_->broadcast_all(proto::play_cb::kDestroyEntities, destroy.data());
        }
        // 拾取：给玩家经验
        for (const auto& collected : xp_events.collected) {
            hub_->send_experience(collected.player_entity_id, collected.xp_value);
            // 拾取音效（entity.experience_orb.pickup，id 163）
            ByteWriter sound;
            net::writers::write_named_sound(sound, 163, proto::sound_category::kBlocks,
                                            static_cast<std::int32_t>(collected.x),
                                            static_cast<std::int32_t>(collected.y),
                                            static_cast<std::int32_t>(collected.z), 0.1f, 1.0f);
            hub_->send_to(collected.player_entity_id, proto::play_cb::kSoundEffect, sound.data());
            // CollectItem 展示飞入玩家动画
            ByteWriter collect;
            net::writers::write_collect_item(collect, collected.orb_entity_id,
                                            collected.player_entity_id, 1);
            hub_->send_to(collected.player_entity_id, proto::play_cb::kCollectItem, collect.data());
        }
    }
    if (mobs_ != nullptr) {
        const std::int32_t radius = std::clamp(config_.view_distance, 2, 8);
        // 目标选择用玩家快照（本 tick 取一次，避免每个生物各扫一遍）
        const auto players = hub_->others(net::kNoExclude);
        // 白天判定：now/50ms = tick，mod 24000 取 0..11999（vanilla 昼夜半周期；世界时间
        // 从进程启动起算的近似，无 doDaylightCycle/时间指令）
        const bool daytime = (now_ms() / 50) % 24000 < 12000;
        const auto mob_events = mobs_->tick(*world_, players, daytime);
        for (const auto& mob : mob_events.moved) {
            const auto old_cpos = world::ChunkPos::from_world(mob.old_x, mob.old_z);
            const auto new_cpos = world::ChunkPos::from_world(mob.x, mob.z);
            if (old_cpos && new_cpos && *old_cpos != *new_cpos) {
                ByteWriter spawn;
                net::writers::encode_spawn_mob(spawn, mob.entity_id, mob.type, mob.x, mob.y, mob.z,
                                               mob.yaw);
                std::vector<std::pair<std::int32_t, Bytes>> pkts;
                pkts.push_back({proto::play_cb::kSpawnMob,
                                Bytes{spawn.data().begin(), spawn.data().end()}});
                if (mob.type == 51) {
                    ByteWriter equip;
                    net::writers::encode_skeleton_bow(equip, mob.entity_id);
                    pkts.push_back({proto::play_cb::kEntityEquipment,
                                    Bytes{equip.data().begin(), equip.data().end()}});
                }
                hub_->transition_entity(old_cpos->x, old_cpos->z, new_cpos->x, new_cpos->z,
                                        radius, mob.entity_id, pkts);
            }
            if (!new_cpos) {
                continue;
            }
            ByteWriter tp;
            net::writers::write_entity_teleport(tp, mob.entity_id, mob.x, mob.y, mob.z, mob.yaw, 0.0f,
                                                true);
            hub_->broadcast_near(new_cpos->x, new_cpos->z, radius, 0, proto::play_cb::kEntityTeleport,
                                 tp.data());
            ByteWriter head;
            net::writers::write_entity_head_look(head, mob.entity_id, mob.yaw);
            hub_->broadcast_near(new_cpos->x, new_cpos->z, radius, 0, proto::play_cb::kEntityHeadLook,
                                 head.data());
        }
        // 生物近战命中：走 hub 邮箱在目标连接线程扣血（受伤状态/击退/致死）
        for (const auto& attack : mob_events.attacks) {
            hub_->send_damage(attack.target_player, attack.damage, attack.x, attack.z);
            // 攻击者的挥臂动画广播给附近玩家（受击者由 UpdateHealth 红闪感知）
            ByteWriter anim;
            net::writers::write_animation(anim, attack.mob_id, 0);
            const auto cpos = world::ChunkPos::from_world(static_cast<std::int32_t>(attack.x),
                                                           static_cast<std::int32_t>(attack.z));
            if (cpos) {
                hub_->broadcast_near(cpos->x, cpos->z, radius, 0, proto::play_cb::kAnimation,
                                     anim.data());
            }
        }
        // 生物死亡：死亡瞬间（despawn=false）播 EntityStatus 3 + 死亡音 + 掉落；
        // 动画结束/despawn（despawn=true）只销毁实体
        for (const auto& death : mob_events.deaths) {
            const auto species = world::mob_type(death.type);
            const auto cpos = world::ChunkPos::from_world(static_cast<std::int32_t>(death.x),
                                                           static_cast<std::int32_t>(death.z));
            if (death.despawn) {
                ByteWriter destroy;
                const std::uint32_t ids[] = {death.mob_id};
                net::writers::write_destroy_entities(destroy, ids);
                if (cpos) {
                    hub_->broadcast_near(cpos->x, cpos->z, radius, 0,
                                         proto::play_cb::kDestroyEntities, destroy.data());
                }
                continue;
            }
            // EntityStatus 3：客户端播倒地死亡动画（实体 20 tick 后由 despawn 事件销毁）
            ByteWriter dead_status;
            net::writers::write_entity_status(dead_status, death.mob_id, 3);
            ByteWriter sound;
            if (species) {
                net::writers::write_named_sound(sound, species->death_sound,
                                                proto::sound_category::kBlocks,
                                                static_cast<std::int32_t>(death.x),
                                                static_cast<std::int32_t>(death.y),
                                                static_cast<std::int32_t>(death.z), 1.0f, 1.0f);
            }
            if (cpos) {
                hub_->broadcast_near(cpos->x, cpos->z, radius, 0, proto::play_cb::kEntityStatus,
                                     dead_status.data());
                if (species) {
                    hub_->broadcast_near(cpos->x, cpos->z, radius, 0, proto::play_cb::kSoundEffect,
                                         sound.data());
                }
            }
            // 掉落（onDeath 立即掉落；玩家击杀与自然环境死亡统一走这里，避免双份）
            if (species && item_drops_ != nullptr) {
                static thread_local std::mt19937 drop_engine{std::random_device{}()};
                for (const auto& drop : species->drops) {
                    if (drop.item_id == 0) {
                        break;
                    }
                    if (drop.chance_percent < 100 &&
                        std::uniform_int_distribution<int>(1, 100)(drop_engine) >
                            drop.chance_percent) {
                        continue;
                    }
                    const auto count =
                        drop.max_count > drop.min_count
                            ? static_cast<std::uint8_t>(std::uniform_int_distribution<int>(
                                  drop.min_count, drop.max_count)(drop_engine))
                            : drop.min_count;
                    if (count == 0) {
                        continue;
                    }
                    const auto [vx, vy, vz] = net::throw_velocity();
                    spawn_drop_world(death.x, death.y, death.z,
                                     item::ItemStack{drop.item_id, count, drop.damage},
                                     vx, vy, vz);
                }
            }
            // 生成经验球
            if (xp_orb_manager_ != nullptr && species && death.experience > 0) {
                xp_orb_manager_->spawn(death.x, death.y, death.z, death.experience);
            }
        }
        // 苦力怕 swell 状态变化：引信中播引信音效 + 白闪 metadata（index 16 VarInt）；
        // 熄灭只广播 metadata -1（vanilla 引信可回退）
        for (const auto& ignition : mob_events.ignitions) {
            ByteWriter meta;
            meta.varint(static_cast<std::int32_t>(ignition.mob_id));
            meta.u8(12);   // EntityCreeper.STATE（defineId 父类链计数：12）
            meta.varint(1);  // VarInt（serializer 序 1）
            meta.varint(static_cast<std::int32_t>(ignition.fuse_state));  // 1 引信中 / -1 熄灭
            meta.u8(0xFF);
            const auto cpos = world::ChunkPos::from_world(static_cast<std::int32_t>(ignition.x),
                                                           static_cast<std::int32_t>(ignition.z));
            if (!cpos) {
                continue;
            }
            if (ignition.fuse_state > 0) {
                ByteWriter sound;
                net::writers::write_named_sound(
                    sound, 173 /*entity.creeper.primed*/, proto::sound_category::kBlocks,
                    static_cast<std::int32_t>(ignition.x), static_cast<std::int32_t>(ignition.y),
                    static_cast<std::int32_t>(ignition.z), 1.0f, 0.5f);  // vanilla 音高 0.5
                hub_->broadcast_near(cpos->x, cpos->z, radius, 0, proto::play_cb::kSoundEffect,
                                     sound.data());
            }
            hub_->broadcast_near(cpos->x, cpos->z, radius, 0, proto::play_cb::kEntityMetadata,
                                 meta.data());
        }
        // 骷髅举弓/收弓：SWINGING_ARMS（索引 12，Boolean 序 7）
        for (const auto& draw : mob_events.draws) {
            ByteWriter meta;
            meta.varint(static_cast<std::int32_t>(draw.mob_id));
            meta.u8(12);
            meta.varint(6);  // Forge DataSerializers：BOOLEAN=6（原版 7=ROTATIONS 会把包读穿）
            meta.u8(draw.drawing ? 1 : 0);
            meta.u8(0xFF);
            const auto cpos = world::ChunkPos::from_world(static_cast<std::int32_t>(draw.x),
                                                           static_cast<std::int32_t>(draw.z));
            if (cpos) {
                hub_->broadcast_near(cpos->x, cpos->z, radius, 0, proto::play_cb::kEntityMetadata,
                                     meta.data());
            }
        }
        // 环境音（闲置哼声）与环境伤害音（火/岩浆/摔落）
        for (const auto& snd : mob_events.sounds) {
            ByteWriter sound;
            net::writers::write_named_sound(sound, snd.sound_id, proto::sound_category::kBlocks,
                                            static_cast<std::int32_t>(snd.x),
                                            static_cast<std::int32_t>(snd.y),
                                            static_cast<std::int32_t>(snd.z), 1.0f, 1.0f);
            const auto cpos = world::ChunkPos::from_world(static_cast<std::int32_t>(snd.x),
                                                           static_cast<std::int32_t>(snd.z));
            if (cpos) {
                hub_->broadcast_near(cpos->x, cpos->z, radius, 0, proto::play_cb::kSoundEffect,
                                     sound.data());
            }
        }
        // 骷髅射箭
        for (const auto& shot : mob_events.shots) {
            fire_arrow(shot);
        }
        // 苦力怕引爆
        for (const auto& explosion : mob_events.explosions) {
            apply_explosion(explosion.x, explosion.y, explosion.z, explosion.power);
        }
        // 自然刷怪生成
        for (const auto& spawn : mob_events.spawns) {
            const auto cpos = world::ChunkPos::from_world(spawn.x, spawn.z);
            if (!cpos) {
                continue;
            }
            ByteWriter writer;
            net::writers::encode_spawn_mob(writer, spawn.entity_id, spawn.type, spawn.x, spawn.y,
                                           spawn.z, spawn.yaw);
            hub_->broadcast_near(cpos->x, cpos->z, radius, 0, proto::play_cb::kSpawnMob,
                                 writer.data());
            if (spawn.type == 51) {
                ByteWriter equip;
                net::writers::encode_skeleton_bow(equip, spawn.entity_id);
                hub_->broadcast_near(cpos->x, cpos->z, radius, 0,
                                     proto::play_cb::kEntityEquipment, equip.data());
            }
        }
    }
    // 箭飞行与命中
    if (projectiles_ != nullptr && world_ != nullptr && hub_ != nullptr && mobs_ != nullptr) {
        const std::int32_t radius = std::clamp(config_.view_distance, 2, 8);
        for (const auto& hit : projectiles_->tick(*world_, *hub_, *mobs_)) {
            // 击退方向用射手位置（vanilla getTrueSource），而非箭的命中点
            if (hit.hit_player) {
                hub_->send_damage(hit.target_player, hit.damage, hit.source_x, hit.source_z);
            } else if (hit.hit_mob) {
                mobs_->damage(hit.target_mob, hit.damage, hit.source_x, hit.source_z);
            }
            ByteWriter destroy;
            const std::uint32_t ids[] = {hit.arrow_id};
            net::writers::write_destroy_entities(destroy, ids);
            const auto cpos = world::ChunkPos::from_world(static_cast<std::int32_t>(hit.x),
                                                           static_cast<std::int32_t>(hit.z));
            if (cpos) {
                hub_->broadcast_near(cpos->x, cpos->z, radius, 0, proto::play_cb::kDestroyEntities,
                                     destroy.data());
            }
        }
    }
}

void Server::apply_item_tick(const net::ItemTickResult& events) {
    const std::int32_t radius = std::clamp(config_.view_distance, 2, 8);
    const auto chunk_of = [](double x, double z) {
        return world::ChunkPos::from_world(x, z);
    };
    // 消失（5 分钟寿命 / 烧毁）：DestroyEntities；岩浆烧毁附燃烧音
    for (const auto& gone : events.destroyed) {
        ByteWriter destroy;
        const std::uint32_t ids[] = {gone.entity_id};
        net::writers::write_destroy_entities(destroy, ids);
        const auto cpos = chunk_of(gone.x, gone.z);
        if (cpos) {
            hub_->broadcast_near(cpos->x, cpos->z, radius, 0, proto::play_cb::kDestroyEntities,
                                 destroy.data());
        }
        if (gone.burn_sound) {
            ByteWriter sound;
            net::writers::write_named_sound(sound, 233 /*entity.generic.burn*/,
                                            proto::sound_category::kBlocks,
                                            static_cast<std::int32_t>(gone.x),
                                            static_cast<std::int32_t>(gone.y),
                                            static_cast<std::int32_t>(gone.z), 0.4f, 1.0f);
            if (cpos) {
                hub_->broadcast_near(cpos->x, cpos->z, radius, 0, proto::play_cb::kSoundEffect,
                                     sound.data());
            }
        }
    }
    // 位置同步：vanilla tracker 每 20 tick 一次校正（EntityTeleport，客户端本地模拟物理）
    for (const auto& move : events.moved) {
        const auto old_cpos = world::ChunkPos::from_world(move.old_x, move.old_z);
        const auto new_cpos = world::ChunkPos::from_world(move.x, move.z);
        if (old_cpos && new_cpos && *old_cpos != *new_cpos) {
            ByteWriter spawn;
            ByteWriter meta;
            net::writers::encode_dropped_item(spawn, meta, move.entity_id, move.x, move.y, move.z,
                                              0.0, 0.0, 0.0, move.stack);
            const std::pair<std::int32_t, Bytes> pkts[] = {
                {proto::play_cb::kSpawnObject, Bytes{spawn.data().begin(), spawn.data().end()}},
                {proto::play_cb::kEntityMetadata, Bytes{meta.data().begin(), meta.data().end()}}
            };
            hub_->transition_entity(old_cpos->x, old_cpos->z, new_cpos->x, new_cpos->z,
                                    radius, move.entity_id, pkts);
        }
        ByteWriter tp;
        net::writers::write_entity_teleport(tp, move.entity_id, move.x, move.y, move.z, 0.0f, 0.0f,
                                            true);
        if (new_cpos) {
            hub_->broadcast_near(new_cpos->x, new_cpos->z, radius, 0, proto::play_cb::kEntityTeleport,
                                 tp.data());
        }
    }
    // 合并：吸收方销毁 + 存活方重发堆叠 metadata（index 6, Slot）
    for (const auto& merged : events.merged) {
        ByteWriter destroy;
        const std::uint32_t ids[] = {merged.victim_id};
        net::writers::write_destroy_entities(destroy, ids);
        ByteWriter meta;
        meta.varint(static_cast<std::int32_t>(merged.survivor_id));
        meta.u8(6);
        meta.varint(5);
        item::write_slot(meta, merged.stack);
        meta.u8(0xFF);
        const auto cpos = chunk_of(merged.x, merged.z);
        if (cpos) {
            hub_->broadcast_near(cpos->x, cpos->z, radius, 0, proto::play_cb::kDestroyEntities,
                                 destroy.data());
            hub_->broadcast_near(cpos->x, cpos->z, radius, 0, proto::play_cb::kEntityMetadata,
                                 meta.data());
        }
    }
    // 岩浆浮起的燃烧音（实体未销毁，仅音效）
    for (const auto& sound_at : events.burn_sounds) {
        ByteWriter sound;
        net::writers::write_named_sound(sound, 233 /*entity.generic.burn*/,
                                        proto::sound_category::kBlocks,
                                        static_cast<std::int32_t>(sound_at.x),
                                        static_cast<std::int32_t>(sound_at.y),
                                        static_cast<std::int32_t>(sound_at.z), 0.4f, 1.0f);
        const auto cpos = chunk_of(sound_at.x, sound_at.z);
        if (cpos) {
            hub_->broadcast_near(cpos->x, cpos->z, radius, 0, proto::play_cb::kSoundEffect,
                                 sound.data());
        }
    }
}

void Server::spawn_drop_world(double x, double y, double z, item::ItemStack stack,
                              double vx, double vy, double vz) {
    if (item_drops_ == nullptr || stack.empty()) {
        return;
    }
    const auto eid = item_drops_->spawn(x, y, z, stack, vx, vy, vz, 10);
    if (eid == 0) {
        return;
    }
    ByteWriter spawn;
    ByteWriter meta;
    net::writers::encode_dropped_item(spawn, meta, eid, x, y, z, vx, vy, vz, stack);
    const auto cpos = world::ChunkPos::from_world(x, z);
    if (!cpos) {
        return;
    }
    const std::int32_t radius = std::clamp(config_.view_distance, 2, 8);
    hub_->broadcast_near(cpos->x, cpos->z, radius, 0, proto::play_cb::kSpawnObject, spawn.data());
    hub_->broadcast_near(cpos->x, cpos->z, radius, 0, proto::play_cb::kEntityMetadata, meta.data());
}

namespace {
std::mt19937& explosion_rng() {
    static thread_local std::mt19937 engine{std::random_device{}()};
    return engine;
}
}  // namespace

void Server::fire_arrow(const net::MobShot& shot) {
    // EntitySkeletonAbstract.a(EntityLiving, float)：
    //   d1 = 目标 y + length/3 - 箭 y；d3 = 水平距离；
    //   arrow.shoot(dx, d1 + d3*0.2, dz, 1.6, 14 - difficulty*4)
    // EntityArrow.shoot：方向归一化 × 1.6，再叠加各轴 nextGaussian*0.0075*inaccuracy
    const double dx = shot.tx - shot.x;
    const double dy = shot.ty - shot.y;
    const double dz = shot.tz - shot.z;
    const double horizontal = std::sqrt(dx * dx + dz * dz);
    const double aim_y = dy + horizontal * 0.2;
    const double len = std::sqrt(dx * dx + aim_y * aim_y + dz * dz);
    constexpr double kArrowSpeed = 1.6;
    static thread_local std::normal_distribution<double> gauss(0.0, 1.0);
    constexpr double kInaccuracy = 6.0;  // 14 - 难度(普通=2)*4
    const double scatter = 0.0075 * kInaccuracy;
    const double vx = (dx / len + gauss(explosion_rng()) * scatter) * kArrowSpeed;
    const double vy = (aim_y / len + gauss(explosion_rng()) * scatter) * kArrowSpeed;
    const double vz = (dz / len + gauss(explosion_rng()) * scatter) * kArrowSpeed;
    // damage 系数 2.0（EntityArrow 默认）：实际伤害在命中时按当时速度计算
    projectiles_->spawn(shot.mob_id, shot.x, shot.y, shot.z, vx, vy, vz, 2.0f);
    const auto arrow_id = projectiles_->snapshot().back().entity_id;

    // EntityArrow.shoot 角度计算：
    // rotationYaw = atan2(vx, vz) * 180 / PI
    // rotationPitch = atan2(vy, horizontal_speed) * 180 / PI
    constexpr double kRadToDeg = 180.0 / 3.14159265358979323846;
    const double horiz_v = std::sqrt(vx * vx + vz * vz);
    const float yaw = static_cast<float>(std::atan2(vx, vz) * kRadToDeg);
    const float pitch = static_cast<float>(std::atan2(vy, horiz_v) * kRadToDeg);
    // 原版 data = 1 + shooter_id（客户端 handleSpawnObject 解析为 data - 1）
    const std::int32_t arrow_data = static_cast<std::int32_t>(shot.mob_id + 1);

    ByteWriter spawn;
    net::writers::write_spawn_object(spawn, arrow_id, kObjectTypeArrow, shot.x, shot.y, shot.z,
                                     yaw, pitch, arrow_data, vx, vy, vz);
    ByteWriter sound;
    net::writers::write_named_sound(sound, 407 /*entity.skeleton.shoot*/,
                                    proto::sound_category::kBlocks,
                                    static_cast<std::int32_t>(shot.x), static_cast<std::int32_t>(shot.y),
                                    static_cast<std::int32_t>(shot.z), 1.0f, 1.0f);
    const auto cpos = world::ChunkPos::from_world(static_cast<std::int32_t>(shot.x),
                                                   static_cast<std::int32_t>(shot.z));
    if (cpos) {
        const std::int32_t radius = std::clamp(config_.view_distance, 2, 8);
        hub_->broadcast_near(cpos->x, cpos->z, radius, 0, proto::play_cb::kSpawnObject,
                             spawn.data());
        hub_->broadcast_near(cpos->x, cpos->z, radius, 0, proto::play_cb::kSoundEffect,
                             sound.data());
    }
}

void Server::apply_explosion(double x, double y, double z, float power) {
    const double power_d = static_cast<double>(power);
    const double blast_radius = power_d * 2.0;  // vanilla f3 = size * 2
    const std::int32_t radius = std::clamp(config_.view_distance, 2, 8);
    static thread_local std::mt19937 rng{std::random_device{}()};

    // 破坏方块：vanilla Explosion.a() 的射线追踪（同 Cuberite Explodinator::DamageBlocks）。
    // 16³ 立方体表面 1352 条射线，强度 = power*(0.7+rand*0.6)，步长 0.3，
    // 每步衰减 (抗性+0.3)*0.3（blocks.hpp::explosion_absorption），强度耗尽即停。
    // 同时收集被毁方块相对爆心的 byte 偏移——1.12.2 的 Explosion 包记录就是它们。
    std::vector<std::array<std::int8_t, 3>> records;
    if (world_ != nullptr && power >= 0.1f) {
        std::uniform_real_distribution<float> unit(0.0f, 1.0f);
        std::uniform_int_distribution<int> drop_chance(1, 100);
        std::unordered_set<std::int64_t> destroyed;
        for (int k = 0; k < 16; ++k) {
            for (int i = 0; i < 16; ++i) {
                for (int j = 0; j < 16; ++j) {
                    if (k != 0 && k != 15 && i != 0 && i != 15 && j != 0 && j != 15) {
                        continue;
                    }
                    double d0 = k / 15.0 * 2.0 - 1.0;
                    double d1 = i / 15.0 * 2.0 - 1.0;
                    double d2 = j / 15.0 * 2.0 - 1.0;
                    const double norm = std::sqrt(d0 * d0 + d1 * d1 + d2 * d2);
                    d0 /= norm;
                    d1 /= norm;
                    d2 /= norm;
                    float intensity = power * (0.7f + unit(rng) * 0.6f);
                    double cx = x;
                    double cy = y;
                    double cz = z;
                    while (intensity > 0.0f) {
                        const auto bx = static_cast<std::int32_t>(std::floor(cx));
                        const auto by = static_cast<std::int32_t>(std::floor(cy));
                        const auto bz = static_cast<std::int32_t>(std::floor(cz));
                        const auto state = world_->block_at(bx, by, bz);
                        if (state != world::kStateAir) {
                            intensity -= (world::explosion_absorption(state) + 0.3f) * 0.3f;
                            if (intensity > 0.0f && by >= 0 && by < 256) {
                                destroyed.insert(world::pack_block_pos(bx, by, bz));
                            }
                        }
                        intensity -= 0.22500001f;
                        cx += d0 * 0.3;
                        cy += d1 * 0.3;
                        cz += d2 * 0.3;
                    }
                }
            }
        }
        // 掉落率 = 1/power（Explosion.a(boolean) 的 yield）
        for (const auto key : destroyed) {
            const auto pos = world::unpack_block_pos(key);
            const auto state = world_->block_at(pos.x, pos.y, pos.z);
            if (state == world::kStateAir) {
                continue;
            }
            world_->set_block(pos.x, pos.y, pos.z, world::kStateAir);
            if (drop_chance(rng) <= static_cast<int>(100.0f / power)) {
                for (const auto& drop : world::block_drops(state, item::ToolInfo{})) {
                    const auto [sx, sy, sz] = net::in_block_spawn_pos(pos.x, pos.y, pos.z);
                    spawn_drop_world(sx, sy, sz,
                                     item::ItemStack{drop.item_id, drop.count, drop.damage});
                }
            }
            ByteWriter change;
            change.position(pos.x, pos.y, pos.z);
            change.varint(0);
            const auto cpos = world::ChunkPos::from_world(pos.x, pos.z);
            if (cpos) {
                hub_->broadcast_near(cpos->x, cpos->z, radius, 0, proto::play_cb::kBlockChange,
                                     change.data());
            }
            records.push_back({static_cast<std::int8_t>(pos.x - static_cast<std::int32_t>(std::floor(x))),
                               static_cast<std::int8_t>(pos.y - static_cast<std::int32_t>(std::floor(y))),
                               static_cast<std::int8_t>(pos.z - static_cast<std::int32_t>(std::floor(z)))});
        }
    }

    // 爆炸音效 + Explosion 包。1.12.2 布局（PacketPlayOutExplosion.b）：位置 f32×3
    // （字段虽声明 double 但按 float 写出！）、半径 f32、记录数 i32、每条记录 byte×3、
    // 玩家动量 f32×3。位置写成 f64 会把半径/记录数读串位——记录数是垃圾值时客户端
    // 按其分配内存直接 OOM。
    ByteWriter sound;
    net::writers::write_named_sound(sound, 231 /*entity.generic.explode*/,
                                    proto::sound_category::kBlocks, static_cast<std::int32_t>(x),
                                    static_cast<std::int32_t>(y), static_cast<std::int32_t>(z),
                                    4.0f, 0.7f);  // vanilla：volume 4.0，pitch (1±0.2)*0.7
    ByteWriter explosion;
    net::writers::write_explosion(explosion, x, y, z, power, records);

    const auto center = world::ChunkPos::from_world(static_cast<std::int32_t>(x),
                                                     static_cast<std::int32_t>(z));
    if (center) {
        hub_->broadcast_near(center->x, center->z, radius, 0, proto::play_cb::kSoundEffect,
                             sound.data());
        hub_->broadcast_near(center->x, center->z, radius, 0, proto::play_cb::kExplosion,
                             explosion.data());
    }

    // 实体伤害：impact = (1 - dist/(power*2)) * 曝光率（包围盒采样点对爆心的无遮挡比例），
    // damage = (impact² + impact)/2 * 7 * power*2 + 1（Explosion.a()）；击退 = 方向 × impact。
    auto blocked = [&](double ex, double ey, double ez, double w, double h) {
        unsigned unobstructed = 0;
        unsigned total = 0;
        for (double px = ex - w / 2; px < ex + w / 2; px += 0.5) {
            for (double py = ey; py < ey + h; py += 0.5) {
                for (double pz = ez - w / 2; pz < ez + w / 2; pz += 0.5) {
                    const double dx = px - x;
                    const double dy = py - y;
                    const double dz = pz - z;
                    const double dist = std::sqrt(dx * dx + dy * dy + dz * dz);
                    const int steps = std::max(1, static_cast<int>(std::ceil(dist / 0.5)));
                    bool clear = true;
                    for (int s = 1; s < steps && clear; ++s) {
                        const double t = static_cast<double>(s) / steps;
                        const auto sx = static_cast<std::int32_t>(std::floor(x + dx * t));
                        const auto sy = static_cast<std::int32_t>(std::floor(y + dy * t));
                        const auto sz = static_cast<std::int32_t>(std::floor(z + dz * t));
                        clear = world_->block_at(sx, sy, sz) == world::kStateAir;
                    }
                    if (clear) {
                        ++unobstructed;
                    }
                    ++total;
                }
            }
        }
        return total == 0 ? 0.0f : static_cast<float>(unobstructed) / static_cast<float>(total);
    };
    if (world_ != nullptr) {
        for (const auto& player : hub_->others(net::kNoExclude)) {
            const double dist = std::sqrt((player.x - x) * (player.x - x) + (player.y - y) * (player.y - y) + (player.z - z) * (player.z - z));
            if (dist > blast_radius) {
                continue;
            }
            const float impact = static_cast<float>(1.0 - dist / blast_radius) *
                                 blocked(player.x, player.y, player.z, 0.6, 1.8);
            if (impact <= 0.0f) {
                continue;
            }
            const double impact_d = static_cast<double>(impact);
            const double dmg = std::floor((impact_d * impact_d + impact_d) / 2.0 * 7.0 *
                                          blast_radius + 1.0);
            hub_->send_damage(player.entity_id, static_cast<float>(dmg), x, z);
        }
        for (const auto& mob : mobs_->snapshot()) {
            const double dist = std::sqrt((mob.pos.x - x) * (mob.pos.x - x) + (mob.pos.y - y) * (mob.pos.y - y) + (mob.pos.z - z) * (mob.pos.z - z));
            if (dist > blast_radius) {
                continue;
            }
            const auto species = world::mob_type(mob.type);
            if (!species) {
                continue;
            }
            const float impact = static_cast<float>(1.0 - dist / blast_radius) *
                                 blocked(mob.pos.x, mob.pos.y, mob.pos.z,
                                         static_cast<double>(species->width),
                                         static_cast<double>(species->height));
            if (impact <= 0.0f) {
                continue;
            }
            const double impact_d = static_cast<double>(impact);
            const double dmg = std::floor((impact_d * impact_d + impact_d) / 2.0 * 7.0 *
                                          blast_radius + 1.0);
            mobs_->damage(mob.entity_id, static_cast<float>(dmg), x, z, impact);
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
