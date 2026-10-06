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
        const int ticks_in_air = 100 - arrow.ttl;  // EntityArrow.ticksInAir
        const double fx = arrow.x;
        const double fy = arrow.y;
        const double fz = arrow.z;
        const double seg_x = arrow.vx;
        const double seg_y = arrow.vy;
        const double seg_z = arrow.vz;
        // rayTraceBlocks：方块命中截断线段
        const auto t_block =
            world::block_ray_hit(world, fx, fy, fz, fx + seg_x, fy + seg_y, fz + seg_z);
        double end_x = fx + seg_x;
        double end_y = fy + seg_y;
        double end_z = fz + seg_z;
        if (t_block) {
            end_x = fx + seg_x * *t_block;
            end_y = fy + seg_y * *t_block;
            end_z = fz + seg_z * *t_block;
        }
        // findEntityOnPath：每个实体盒 grow(0.3) 做线段拦截，取离起点最近者；
        // 主人在空气中 5 tick 内免疫（shootingEntity 例外分支）
        bool found = false;
        bool player_hit = false;
        std::uint32_t target = 0;
        double best_t = 0.0;
        const auto consider = [&](const world::Aabb& raw_box, bool is_owner, bool is_player,
                                  std::uint32_t id) {
            if (is_owner && ticks_in_air < 5) {
                return;
            }
            const world::Aabb box{raw_box.min_x - 0.3, raw_box.min_y - 0.3, raw_box.min_z - 0.3,
                                  raw_box.max_x + 0.3, raw_box.max_y + 0.3, raw_box.max_z + 0.3};
            const auto t = world::segment_aabb_intercept(box, fx, fy, fz, end_x, end_y, end_z);
            if (t && (!found || *t < best_t)) {
                found = true;
                player_hit = is_player;
                target = id;
                best_t = *t;
            }
        };
        for (const auto& player : players) {
            consider(world::entity_box(player.x, player.y, player.z, 0.6f, 1.8f),
                     player.entity_id == arrow.owner_id, true, player.entity_id);
        }
        for (const auto& mob : mobs_snapshot) {
            const auto species = world::mob_type(mob.type);
            if (!species) {
                continue;
            }
            consider(world::entity_box(mob.pos.x, mob.pos.y, mob.pos.z, species->width,
                                       species->height),
                     mob.entity_id == arrow.owner_id, false, mob.entity_id);
        }

        const auto speed =
            static_cast<float>(std::sqrt(seg_x * seg_x + seg_y * seg_y + seg_z * seg_z));
        if (found) {
            const float damage = std::ceil(speed * arrow.damage);
            hits.push_back(ArrowHit{arrow.entity_id, player_hit, target, !player_hit, target,
                                    fx + (end_x - fx) * best_t, fy + (end_y - fy) * best_t,
                                    fz + (end_z - fz) * best_t, damage});
            arrows_[i] = arrows_.back();
            arrows_.pop_back();
            continue;
        }
        if (t_block) {
            hits.push_back(ArrowHit{arrow.entity_id, false, 0, false, 0, end_x, end_y, end_z});
            arrows_[i] = arrows_.back();
            arrows_.pop_back();
            continue;
        }
        // 未命中：pos += motion → 阻力（水 0.6 / 空气 0.99）→ 重力 0.05（vanilla 顺序）
        arrow.x = end_x;
        arrow.y = end_y;
        arrow.z = end_z;
        const auto mid = world::block_id(world.block_at(
            static_cast<std::int32_t>(std::floor(arrow.x)),
            static_cast<std::int32_t>(std::floor(arrow.y)),
            static_cast<std::int32_t>(std::floor(arrow.z))));
        const double drag = mid == 8 || mid == 9 ? 0.6 : 0.99;
        arrow.vx *= drag;
        arrow.vy *= drag;
        arrow.vz *= drag;
        arrow.vy -= 0.05;
        if (arrow.ttl <= 0 || arrow.y < -64.0) {
            arrows_[i] = arrows_.back();
            arrows_.pop_back();
        } else {
            ++i;
        }
    }
    return hits;
}

}  // namespace cyane::net
