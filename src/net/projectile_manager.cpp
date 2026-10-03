#include "cyane/net/projectile_manager.hpp"

#include <algorithm>
#include <cmath>

#include "cyane/net/mob_manager.hpp"
#include "cyane/net/player_hub.hpp"
#include "cyane/world/world.hpp"

namespace cyane::net {

void ProjectileManager::spawn(std::uint32_t owner_id, double x, double y, double z, double vx,
                              double vy, double vz, float damage) {
    std::lock_guard<std::mutex> lock{mutex_};
    arrows_.push_back(Arrow{entity::allocate_entity_id(), owner_id, x, y, z, vx, vy, vz, damage,
                            100});
}

std::vector<ArrowHit> ProjectileManager::tick(world::World& world, const PlayerHub& hub,
                                              MobManager& mobs) {
    std::vector<ArrowHit> hits;
    const auto players = hub.others(kNoExclude);
    const auto mobs_snapshot = mobs.snapshot();
    std::lock_guard<std::mutex> lock{mutex_};
    for (std::size_t i = 0; i < arrows_.size();) {
        Arrow& arrow = arrows_[i];
        --arrow.ttl;
        // 分小步推进：高速箭不打穿 1 格厚的墙
        const double speed =
            std::sqrt(arrow.vx * arrow.vx + arrow.vy * arrow.vy + arrow.vz * arrow.vz);
        const int steps = std::max(1, static_cast<int>(std::ceil(speed * 2.0)));
        const double dt = 1.0 / steps;
        bool removed = false;
        for (int s = 0; s < steps && !removed; ++s) {
            arrow.x += arrow.vx * dt;
            arrow.y += arrow.vy * dt;
            arrow.z += arrow.vz * dt;
            arrow.vy += kGravity * dt;
            // 方块命中
            if (world::is_solid(world.block_at(static_cast<std::int32_t>(std::floor(arrow.x)),
                                               static_cast<std::int32_t>(std::floor(arrow.y)),
                                               static_cast<std::int32_t>(std::floor(arrow.z))))) {
                hits.push_back(ArrowHit{arrow.entity_id, false, 0, false, 0, arrow.x, arrow.y,
                                        arrow.z});
                removed = true;
                break;
            }
            // 玩家命中：箭到玩家身体中段的点距
            for (const auto& player : players) {
                if (player.entity_id == arrow.owner_id) {
                    continue;
                }
                const double dx = player.x - arrow.x;
                const double dy = (player.y + 0.9) - arrow.y;
                const double dz = player.z - arrow.z;
                if (dx * dx + dy * dy + dz * dz <= kHitRadiusSq) {
                    hits.push_back(ArrowHit{arrow.entity_id, true, player.entity_id, false, 0,
                                            arrow.x, arrow.y, arrow.z});
                    removed = true;
                    break;
                }
            }
            if (removed) {
                break;
            }
            // 生物命中
            for (const auto& mob : mobs_snapshot) {
                if (mob.entity_id == arrow.owner_id) {
                    continue;
                }
                const auto species = world::mob_type(mob.type);
                if (!species) {
                    continue;
                }
                const double dx = mob.pos.x - arrow.x;
                const double dy = (mob.pos.y + static_cast<double>(species->height) * 0.5) - arrow.y;
                const double dz = mob.pos.z - arrow.z;
                const double r = static_cast<double>(species->width) * 0.5 + 0.3;
                if (dx * dx + dy * dy + dz * dz <= r * r) {
                    hits.push_back(ArrowHit{arrow.entity_id, false, 0, true, mob.entity_id,
                                            arrow.x, arrow.y, arrow.z});
                    removed = true;
                    break;
                }
            }
        }
        if (removed || arrow.ttl <= 0 || arrow.y < -64.0) {
            arrows_[i] = arrows_.back();
            arrows_.pop_back();
        } else {
            ++i;
        }
    }
    return hits;
}

}  // namespace cyane::net
