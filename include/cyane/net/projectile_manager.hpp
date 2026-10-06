#pragma once

#include <cstdint>
#include <mutex>
#include <vector>

#include "cyane/entity/player_manager.hpp"

namespace cyane::world {
class World;
}

namespace cyane::net {

class PlayerHub;
class MobManager;

// 飞行中的箭：位置 + 速度 + 主人 + 剩余寿命。
// 由 Server::tick 驱动（每 tick 重力 + 位移 + 命中检测），事件由 Server 落地。
struct Arrow {
    std::uint32_t entity_id{0};
    std::uint32_t owner_id{0};  // 射手（骷髅实体 id；玩家射击时为玩家 id）
    double x{0.0};
    double y{0.0};
    double z{0.0};
    double vx{0.0};
    double vy{0.0};
    double vz{0.0};
    float damage{2.0f};
    std::int32_t ttl{100};  // 5 秒自毁上限
};

// 箭的命中结果：Server 据此结算伤害并广播销毁。
// damage = ceil(命中时刻速度 × damage 系数)（EntityArrow.onHit，原版在命中时按当前速度算）
struct ArrowHit {
    std::uint32_t arrow_id{0};
    bool hit_player{false};      // 命中玩家（target_player 有效）
    std::uint32_t target_player{0};
    bool hit_mob{false};         // 命中生物（target_mob 有效）
    std::uint32_t target_mob{0};
    double x{0.0};
    double y{0.0};
    double z{0.0};
    float damage{0.0f};
};

class ProjectileManager {
public:
    // 生成一支箭（起点为骷髅胸口，速度已含弹道补偿）
    void spawn(std::uint32_t owner_id, double x, double y, double z, double vx, double vy,
               double vz, float damage);

    // 推进所有箭一个 tick：返回命中事件（命中后箭即移除）。
    // EntityArrow.onUpdate 逐行转写：先整段线段做方块射线（rayTraceBlocks），
    // 再对截断后的线段做实体 AABB 拦截（findEntityOnPath：实体盒 grow 0.3、六面拦截、
    // 取最近者，主人 5 tick 内免疫），命中即结算；未命中才 pos += motion → ×0.99 阻力 →
    // motionY −= 0.05 重力。
    [[nodiscard]] std::vector<ArrowHit> tick(world::World& world, const PlayerHub& hub,
                                             MobManager& mobs);

    // 位置快照（新登入玩家补发用——本服务端箭寿命短，直接不补发：数量小、5 秒内消失）
    [[nodiscard]] std::vector<Arrow> snapshot() const {
        std::lock_guard<std::mutex> lock{mutex_};
        return arrows_;
    }

    [[nodiscard]] std::size_t size() const {
        std::lock_guard<std::mutex> lock{mutex_};
        return arrows_.size();
    }

private:
    mutable std::mutex mutex_;
    std::vector<Arrow> arrows_;
};

}  // namespace cyane::net
