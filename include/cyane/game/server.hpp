#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "cyane/core/config.hpp"
#include "cyane/core/error.hpp"
#include "cyane/core/thread_pool.hpp"
#include "cyane/core/time.hpp"
#include "cyane/entity/player_manager.hpp"
#include "cyane/game/op_manager.hpp"
#include "cyane/game/player_data.hpp"
#include "cyane/game/world_persistence.hpp"
#include "cyane/item/crafting.hpp"
#include "cyane/net/block_ticks.hpp"
#include "cyane/net/crafting_table_store.hpp"
#include "cyane/net/mob_manager.hpp"
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
    std::string game_mode{"creative"};
    std::string op_file{"config/ops.json"};
    std::string recipe_file{"config/recipes.toml"};
    std::string player_data_dir{"world/playerdata"};  // 原版布局：<world>/playerdata/<uuid>.dat
    int autosave_interval{300};  // 秒；0 = 关闭

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

    // 控制台命令支持
    [[nodiscard]] double current_tps() const noexcept { return stats_.tps(); }
    // 在线玩家数 = 已进入 play 阶段的玩家（hub 注册表），不含 status ping 握手连接
    [[nodiscard]] std::uint64_t online_players() const noexcept {
        return hub_ != nullptr ? hub_->size() : 0;
    }
    // 以系统身份向所有在线玩家广播一条聊天消息（控制台 say 命令）
    void broadcast_system_message(std::string_view message);
    // 根据玩家名杀死一名在线玩家（控制台 /kill 命令）
    [[nodiscard]] bool kill_player_by_name(std::string_view name);
    // OP 管理
    [[nodiscard]] const game::OpManager& op_manager() const noexcept { return *op_manager_; }
    [[nodiscard]] bool set_player_gamemode(std::string_view name, std::string_view mode);
    [[nodiscard]] bool op_player(std::string_view name);
    [[nodiscard]] bool deop_player(std::string_view name);
    // 在线玩家名列表（控制台 list 命令）
    [[nodiscard]] std::vector<std::string> player_names() const;
    // 世界存档：立即把方块编辑与箱子/熔炉落盘（控制台 save 命令）
    void save_world_now();
    void request_save() { save_pending_.store(true, std::memory_order_relaxed); }

private:
    explicit Server(ServerConfig config);

    void tick();

    ServerConfig config_;
    std::unique_ptr<game::ServerStatus> status_;
    std::unique_ptr<entity::PlayerManager> player_manager_;
    std::unique_ptr<net::PlayerHub> hub_;
    std::unique_ptr<net::BlockTicks> block_ticks_;
    std::unique_ptr<net::ItemDropManager> item_drops_;
    std::unique_ptr<net::ContainerStore> containers_;
    std::unique_ptr<net::FurnaceStore> furnaces_;
    std::unique_ptr<world::CraftingTableStore> crafting_tables_;
    std::unique_ptr<net::MobManager> mobs_;
    std::unique_ptr<game::OpManager> op_manager_;
    std::unique_ptr<item::CraftingRegistry> crafting_;
    std::unique_ptr<world::World> world_;
    std::unique_ptr<game::WorldPersistence> persistence_;
    std::unique_ptr<net::NetService> network_;
    std::unique_ptr<ThreadPool> workers_;
    TickStats stats_;
    std::atomic<bool> running_{true};
    std::unique_ptr<game::PlayerDataStore> player_data_store_;
    std::uint64_t ticks_since_save_{0};
    std::atomic<bool> save_pending_{false};
    std::chrono::steady_clock::time_point last_save_time_{std::chrono::steady_clock::now()};
    static constexpr std::uint64_t kMinSaveIntervalMs = 5000;  // 断开触发保存的最小间隔
};

}
