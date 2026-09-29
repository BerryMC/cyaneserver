#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

#include "cyane/core/error.hpp"
#include "cyane/item/item_stack.hpp"
#include "cyane/item/player_inventory.hpp"
#include "cyane/proto/play_fields.hpp"

namespace cyane::game {

// 玩家持久化数据：位置、朝向、游戏模式、血量与 46 格背包。
// 落盘格式为原版 1.12.2 playerdata/<uuid>.dat（gzip NBT），字段名/布局与其一致。
struct PlayerData {
    std::string username;
    std::string uuid_with_dashes;
    double x{0.5};
    double y{4.0};
    double z{0.5};
    float yaw{0.0f};
    float pitch{0.0f};
    std::uint8_t game_mode{proto::game_mode::kCreative};
    float health{20.0f};
    std::uint8_t selected_slot{0};
    // 46 格背包，与 item::PlayerInventory 窗口布局一致（0 结果/1-4 合成/5-8 护甲/9-35 主/36-44 热区/45 副手）
    std::array<item::ItemStack, item::PlayerInventory::kSlotCount> inventory{};
};

// 玩家数据持久化存储：<dir>/<uuid>.dat（原版 gzip NBT）。
// 首次载入遇到旧版 <uuid>.json 时自动迁移为 .dat。
class PlayerDataStore {
public:
    PlayerDataStore() = default;
    PlayerDataStore(const PlayerDataStore&) = delete;
    PlayerDataStore& operator=(const PlayerDataStore&) = delete;

    // 按 UUID 加载；无存档时返回默认数据（username/default_game_mode 填入结果）
    [[nodiscard]] PlayerData load_or_default(std::string_view uuid_with_dashes,
                                             std::string_view username,
                                             std::uint8_t default_game_mode) const;

    // 原版 .dat 落盘（tmp + rename 原子替换）
    [[nodiscard]] Result<void> save(const PlayerData& data) const;

    void remove(std::string_view uuid_with_dashes);

    // 列出所有有数据的玩家 UUID（.dat 与遗留 .json）
    [[nodiscard]] std::vector<std::string> list_players() const;

    void set_dir(std::string_view dir);

    [[nodiscard]] const std::string& dir() const noexcept { return dir_; }

private:
    // 假定 mutex_ 已被调用方持有（load_or_default 的迁移分支复用）
    [[nodiscard]] Result<void> save_locked(const PlayerData& data) const;

    std::string dir_{"world/playerdata"};
    mutable std::mutex mutex_;
};

} // namespace cyane::game