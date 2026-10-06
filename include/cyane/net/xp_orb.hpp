#pragma once

#include <cstdint>
#include <mutex>
#include <span>
#include <vector>

#include "cyane/entity/player_manager.hpp"
#include "cyane/net/player_hub.hpp"
#include "cyane/world/physics.hpp"
#include "cyane/world/world.hpp"

namespace cyane::net {

// 经验球存档状态
struct XPOrbState {
    double x{0.0};
    double y{0.0};
    double z{0.0};
    float velocity_x{0.0f};
    float velocity_y{0.0f};
    float velocity_z{0.0f};
    int value{0};
    int age{0};
    int delay_before_can_pickup{0};
    int sync_x{0};
    int sync_y{0};
    int sync_z{0};
    int sync_yaw{0};
    bool on_ground{false};
};

// 经验球实体（内存中）
struct XPOrb : public XPOrbState {
    std::uint32_t entity_id{0};
};

// 经验球 tick 的事件：Server 落地为广播（XPOrbManager 不依赖连接与邮箱）
struct XPSpawned {
    std::uint32_t entity_id{0};
    double x{0.0};
    double y{0.0};
    double z{0.0};
    std::int16_t value{0};
};
struct XPTeleport {
    std::uint32_t entity_id{0};
    double x{0.0};
    double y{0.0};
    double z{0.0};
};
struct XPDestroyed {
    std::uint32_t entity_id{0};
};
struct XPCollected {
    std::uint32_t player_entity_id{0};
    std::uint32_t orb_entity_id{0};
    int xp_value{0};
};

struct XPTickResult {
    std::vector<XPSpawned> spawned;
    std::vector<XPTeleport> teleported;
    std::vector<XPDestroyed> destroyed;
    std::vector<XPCollected> collected;
};

// 线程安全的经验球登记表。Server::tick 驱动物理，tick 返回事件供广播。
class XPOrbManager {
public:
    XPOrbManager() = default;

    // 在 (x,y,z) 生成一个经验球，value 为经验点数；返回实体 id
    std::uint32_t spawn(double x, double y, double z, int value);

    // 推进所有经验球一个 tick（EntityExperienceOrb.onUpdate 逐行转写）
    [[nodiscard]] XPTickResult tick(world::World& world, std::span<const PlayerSnapshot> players);

    // 从存档恢复
    void restore(std::span<const XPOrbState> orbs);

    // 全量快照（保存存档用）
    [[nodiscard]] std::vector<XPOrbState> all_orbs() const;

    // 当前所有经验球快照（新玩家/区块补发）
    [[nodiscard]] std::vector<XPOrb> snapshot() const;

private:
    mutable std::mutex mutex_;
    std::vector<XPOrb> orbs_;
};

}  // namespace cyane::net
