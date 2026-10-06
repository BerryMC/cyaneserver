#include "cyane/net/xp_orb.hpp"

#include "cyane/entity/player_manager.hpp"
#include "cyane/net/packet_writers.hpp"

namespace cyane::net {

std::uint32_t XPOrbManager::spawn(double x, double y, double z, int value) {
    if (value <= 0) {
        return 0;
    }
    const std::uint32_t id = entity::allocate_entity_id();
    std::lock_guard<std::mutex> lock{mutex_};
    XPOrb orb;
    orb.entity_id = id;
    orb.x = x;
    orb.y = y;
    orb.z = z;
    orb.velocity_x = 0.0f;
    orb.velocity_y = 0.0f;
    orb.velocity_z = 0.0f;
    orb.value = value;
    orb.age = 0;
    orb.delay_before_can_pickup = 20; // vanilla 初始延迟 20 tick（1秒）
    orb.sync_x = static_cast<int>(std::floor(x * 32.0));
    orb.sync_y = static_cast<int>(std::floor(y * 32.0));
    orb.sync_z = static_cast<int>(std::floor(z * 32.0));
    orb.sync_yaw = 0;
    orbs_.push_back(std::move(orb));
    return id;
}

[[nodiscard]] XPTickResult XPOrbManager::tick(world::World& world, std::span<const PlayerSnapshot> players) {
    std::lock_guard<std::mutex> lock(mutex_);

    XPTickResult result;

    for (auto it = orbs_.begin(); it != orbs_.end();) {
        // 重力
        int bx = static_cast<int>(std::floor(it->x));
        int by = static_cast<int>(std::floor(it->y));
        int bz = static_cast<int>(std::floor(it->z));
        bool in_water = (world.block_at(bx, by, bz) == 8 || world.block_at(bx, by, bz) == 9);
        if (!in_water) {
            it->velocity_y -= 0.03f; // 重力 0.03/tick
        }

        // 空气阻力
        it->velocity_x *= 0.98f;
        it->velocity_y *= 0.98f;
        it->velocity_z *= 0.98f;

        // 地面摩擦和落地反弹
        auto state = world.block_at(bx, by, bz);
        double slipperiness = world::block_slipperiness(state);
        bool on_ground = world::is_solid(world.block_at(bx, by - 1, bz));

        if (on_ground) {
            it->velocity_x *= static_cast<float>(slipperiness) * 0.91f;
            it->velocity_y = 0.0f;
            it->velocity_z *= static_cast<float>(slipperiness) * 0.91f;
            if (it->velocity_y < 0.0f) {
                it->velocity_y = -it->velocity_y * -0.9f;
            }
        } else {
            it->velocity_x *= static_cast<float>(slipperiness) * 0.91f;
            it->velocity_z *= static_cast<float>(slipperiness) * 0.91f;
        }

        // 更新位置（使用碰撞检测）
        auto box = world::entity_box(it->x, it->y, it->z, 0.25f, 0.25f);
        double dx = it->velocity_x;
        double dy = it->velocity_y;
        double dz = it->velocity_z;
        const auto outcome = world::move_with_collision(world, box, dx, dy, dz, 0.0);
        it->x = (box.min_x + box.max_x) * 0.5;
        it->y = box.min_y;
        it->z = (box.min_z + box.max_z) * 0.5;
        it->on_ground = outcome.on_ground;

        // 更新同步坐标（每 20 tick 同步一次）
        if (it->age % 20 == 0) {
            result.teleported.push_back({
                .entity_id = it->entity_id,
                .x = it->x,
                .y = it->y,
                .z = it->z,
            });
        }

        // 更新 age 和 delay
        ++it->age;
        if (it->delay_before_can_pickup > 0) {
            --it->delay_before_can_pickup;
        }

        // 检测拾取（AABB 相交）
        bool collected = false;
        std::uint32_t collector_id = 0;
        for (const auto& player : players) {
            if (player.x == 0.0 && player.y == 0.0 && player.z == 0.0) {
                continue;
            }
            auto player_box = world::entity_box(player.x, player.y, player.z, 0.6f, 1.8f);
            auto orb_box = world::entity_box(it->x, it->y, it->z, 0.25f, 0.25f);
            bool intersects = !(orb_box.max_x < player_box.min_x ||
                              orb_box.min_x > player_box.max_x ||
                              orb_box.max_y < player_box.min_y ||
                              orb_box.min_y > player_box.max_y ||
                              orb_box.max_z < player_box.min_z ||
                              orb_box.min_z > player_box.max_z);
            if (intersects && it->delay_before_can_pickup == 0) {
                collected = true;
                collector_id = player.entity_id;
                break;
            }
        }

        if (collected) {
            std::uint32_t orb_id = it->entity_id;
            int xp_value = it->value;
            it = orbs_.erase(it);
            result.collected.push_back({
                .player_entity_id = collector_id,
                .orb_entity_id = orb_id,
                .xp_value = xp_value,
            });
            result.destroyed.push_back({
                .entity_id = orb_id,
            });
        } else {
            ++it;
        }
    }

    return result;
}

void XPOrbManager::restore(std::span<const XPOrbState> orbs) {
    std::lock_guard<std::mutex> lock(mutex_);
    orbs_.clear();
    orbs_.reserve(orbs.size());
    for (const auto& state : orbs) {
        XPOrb orb;
        orb.entity_id = entity::allocate_entity_id();
        orb.x = state.x;
        orb.y = state.y;
        orb.z = state.z;
        orb.velocity_x = state.velocity_x;
        orb.velocity_y = state.velocity_y;
        orb.velocity_z = state.velocity_z;
        orb.value = state.value;
        orb.age = state.age;
        orb.delay_before_can_pickup = state.delay_before_can_pickup;
        orb.sync_x = state.sync_x;
        orb.sync_y = state.sync_y;
        orb.sync_z = state.sync_z;
        orb.sync_yaw = state.sync_yaw;
        orbs_.push_back(std::move(orb));
    }
}

[[nodiscard]] std::vector<XPOrbState> XPOrbManager::all_orbs() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<XPOrbState> out;
    out.reserve(orbs_.size());
    for (const auto& orb : orbs_) {
        out.push_back(static_cast<const XPOrbState&>(orb));
    }
    return out;
}

[[nodiscard]] std::vector<XPOrb> XPOrbManager::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return orbs_;
}

} // namespace cyane::net
