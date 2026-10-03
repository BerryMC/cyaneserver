#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "cyane/entity/player_entity.hpp"
#include "cyane/entity/player_manager.hpp"
#include "cyane/net/player_hub.hpp"
#include "cyane/world/mob_types.hpp"
#include "cyane/world/physics.hpp"
#include "cyane/world/world.hpp"

namespace cyane::net {

// 角度 float(度) → 1/256 圈的字节角（协议线格式）
[[nodiscard]] inline std::uint8_t angle_byte(float degrees) noexcept {
    return static_cast<std::uint8_t>(static_cast<int>(degrees * 256.0f / 360.0f) & 0xFF);
}

// AI 状态（被动：待机/漫游/受击逃窜；敌对：追击）
enum class MobAi : std::uint8_t {
    idle,
    wander,
    chase,
    retreat,
};

// 生物：状态 + 物理 + AI。物种属性按 type 从 world::mob_type() 取，不在此重复存储。
struct Mob {
    std::uint32_t entity_id{0};
    std::int32_t type{90};
    entity::Position pos{};
    float health{20.0f};
    // 物理：竖直速度 + 击退动量（每 tick 衰减）
    double velocity_y{0.0};
    double velocity_x{0.0};
    double velocity_z{0.0};
    bool on_ground{false};
    // AI
    MobAi ai{MobAi::idle};
    std::int32_t state_ticks{0};   // 当前状态剩余 tick
    std::int32_t scan_ticks{0};    // 目标扫描节流
    double dir_x{0.0};
    double dir_z{0.0};
    double home_x{0.0};            // 生成点（漫游半径锚点）
    double home_z{0.0};
    std::uint32_t target_player{0};  // 敌对目标玩家实体 id（0 = 无）
    std::int32_t attack_cooldown{0};
    std::int32_t fuse_ticks{-1};   // 苦力怕引信（-1 = 未点燃）
};

// 骷髅射出的箭：起点与目标（Server 负责生成投射物实体）
struct MobShot {
    std::uint32_t mob_id{0};
    std::uint32_t target_player{0};
    double x{0.0};
    double y{0.0};
    double z{0.0};
    double tx{0.0};
    double ty{0.0};
    double tz{0.0};
};

struct MobExplosion {
    double x{0.0};
    double y{0.0};
    double z{0.0};
    float power{3.0f};  // vanilla 苦力怕 explosionRadius=3
};

// 持久化形状：类型 + 位置 + 朝向 + 血量（存档 Entities 列表）
struct MobState {
    std::uint8_t type{90};
    double x{0.0};
    double y{0.0};
    double z{0.0};
    float yaw{0.0f};
    float pitch{0.0f};
    float health{20.0f};
};

// 本 tick 的事件：由 Server 落地成广播/伤害/掉落（MobManager 不依赖连接与邮箱）
struct MobMove {
    std::uint32_t entity_id{0};
    double x{0.0};
    double y{0.0};
    double z{0.0};
    float yaw{0.0f};
};

struct MobAttack {
    std::uint32_t mob_id{0};
    std::uint32_t target_player{0};
    float damage{0.0f};
    double x{0.0};
    double z{0.0};
};

struct MobDeath {
    std::uint32_t mob_id{0};
    std::int32_t type{90};
    double x{0.0};
    double y{0.0};
    double z{0.0};
};

struct MobTickResult {
    std::vector<MobMove> moved;
    std::vector<MobAttack> attacks;
    std::vector<MobDeath> deaths;
    std::vector<MobShot> shots;        // 骷髅射箭
    std::vector<MobExplosion> explosions;  // 苦力怕引爆（自爆即死亡）
};

// 受伤结果：是否命中、是否致死、剩余血量
struct MobHurt {
    bool found{false};
    bool died{false};
    float health{0.0f};
    double x{0.0};
    double y{0.0};
    double z{0.0};
};

// 生物登记表：Server::tick 驱动 AI 与物理（20Hz），连接线程读快照补发。
class MobManager {
public:
    // 在中心点附近随机生成 count 只被动生物，落在地表（world 提供 surface_y）
    void spawn_passive(std::size_t count, world::World& world, double center_x, double center_z);

    // 按类型在指定位置生成一只生物，返回实体 id（未知类型返回 0）
    std::uint32_t spawn(std::int32_t type, double x, double y, double z, float yaw);

    // 造成伤害（含击退方向）：死亡时从表里移除并回报坐标
    MobHurt damage(std::uint32_t id, float amount, double from_x, double from_z);

    // 推进 AI 与物理一个 tick
    [[nodiscard]] MobTickResult tick(world::World& world,
                                     std::span<const PlayerSnapshot> players);

    [[nodiscard]] std::vector<Mob> snapshot() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return mobs_;
    }

    [[nodiscard]] std::optional<Mob> by_id(std::uint32_t id) const {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& m : mobs_) {
            if (m.entity_id == id) {
                return m;
            }
        }
        return std::nullopt;
    }

    // 从存档恢复生物（分配新实体 id，AI 重置为待机）
    void restore(std::span<const MobState> restored) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& s : restored) {
            const auto species = world::mob_type(s.type);
            if (!species) {
                continue;  // 不认识的物种不复活（下次保存时磁盘副本会被清掉）
            }
            Mob m;
            m.entity_id = entity::allocate_entity_id();
            m.type = s.type;
            m.pos.x = s.x;
            m.pos.y = s.y;
            m.pos.z = s.z;
            m.pos.yaw = s.yaw;
            m.pos.pitch = s.pitch;
            m.health = s.health > 0.0f ? s.health : species->health;
            m.state_ticks = 20;
            mobs_.push_back(std::move(m));
        }
    }

    // 全量快照（保存存档用）
    [[nodiscard]] std::vector<MobState> all_mobs() const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<MobState> out;
        out.reserve(mobs_.size());
        for (const auto& m : mobs_) {
            out.push_back(MobState{static_cast<std::uint8_t>(m.type), m.pos.x, m.pos.y, m.pos.z,
                                   m.pos.yaw, m.pos.pitch, m.health});
        }
        return out;
    }

    // 移除生物（返回 true 表示存在且已移除）
    bool remove(std::uint32_t id) {
        std::lock_guard<std::mutex> lock(mutex_);
        return remove_locked(id);
    }

    [[nodiscard]] std::size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return mobs_.size();
    }

    static constexpr double kWanderSpeedScale = 1.0;  // 漫游用物种速度
    static constexpr double kWanderRadius = 24.0;     // 距生成点最大半径
    static constexpr double kRetreatSpeedScale = 1.6; // 受击逃窜加速

private:
    [[nodiscard]] bool remove_locked(std::uint32_t id);

    mutable std::mutex mutex_;
    std::vector<Mob> mobs_;
};

}  // namespace cyane::net
