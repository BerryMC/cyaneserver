#include "cyane/net/mob_manager.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <random>

#include "cyane/world/mob_types.hpp"

namespace cyane::net {
namespace {

constexpr double kPi = 3.14159265358979323846;

// 1.12.2 被动生物类型 id
constexpr std::array<std::int32_t, 4> kPassiveTypes = {90, 91, 92, 93};  // 猪 羊 牛 鸡
constexpr std::int32_t kHostileScanInterval = 10;  // 目标扫描节流：每 0.5s
constexpr std::int32_t kAttackIntervalTicks = 20;  // 近战挥击冷却（vanilla 20 tick）
constexpr std::int32_t kFuseTicks = 30;            // 苦力怕引信（EntityCreeper.maxFuseTicks）
constexpr std::int32_t kShootIntervalTicks = 40;   // 骷髅射箭间隔（dm(): 非困难难度 b0=40）

std::mt19937& rng() {
    thread_local std::mt19937 engine{std::random_device{}()};
    return engine;
}

[[nodiscard]] double rand01() {
    return std::uniform_real_distribution<double>(0.0, 1.0)(rng());
}

[[nodiscard]] std::int32_t rand_ticks(std::int32_t lo, std::int32_t hi) {
    return std::uniform_int_distribution<std::int32_t>(lo, hi)(rng());
}

// 朝 (dir_x, dir_z) 的水平朝向（vanilla：0° 朝 +Z，90° 朝 -X）
[[nodiscard]] float yaw_for(double dir_x, double dir_z) noexcept {
    return static_cast<float>(std::atan2(-dir_x, dir_z) * 180.0 / kPi);
}

// EntityLivingBase.hasLineOfSight：眼到眼射线被实心方块遮挡即不可见（0.5 步进采样）
[[nodiscard]] bool line_clear(world::World& world, double x0, double y0, double z0, double x1,
                              double y1, double z1) {
    const double dx = x1 - x0;
    const double dy = y1 - y0;
    const double dz = z1 - z0;
    const double dist = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (dist < 1e-6) {
        return true;
    }
    const int steps = std::max(1, static_cast<int>(std::ceil(dist / 0.5)));
    for (int s = 1; s < steps; ++s) {
        const double t = static_cast<double>(s) / steps;
        const auto bx = static_cast<std::int32_t>(std::floor(x0 + dx * t));
        const auto by = static_cast<std::int32_t>(std::floor(y0 + dy * t));
        const auto bz = static_cast<std::int32_t>(std::floor(z0 + dz * t));
        if (world::is_solid(world.block_at(bx, by, bz))) {
            return false;
        }
    }
    return true;
}

// EntityLiving.faceEntity(30,30)：转向目标每 tick 不超过 30°
[[nodiscard]] float yaw_towards(float current, double dir_x, double dir_z) {
    const float target = yaw_for(dir_x, dir_z);
    float diff = std::fmod(target - current + 540.0f, 360.0f) - 180.0f;
    if (diff > 30.0f) {
        diff = 30.0f;
    }
    if (diff < -30.0f) {
        diff = -30.0f;
    }
    return current + diff;
}

[[nodiscard]] std::uint32_t nearest_hostile_target(std::span<const PlayerSnapshot> players,
                                                   const Mob& mob, double range) {
    std::uint32_t best = 0;
    double best_sq = range * range;
    for (const auto& player : players) {
        const double dy = std::abs(player.y - mob.pos.y);
        if (dy > 4.0) {
            continue;
        }
        const double dx = player.x - mob.pos.x;
        const double dz = player.z - mob.pos.z;
        const double dist_sq = dx * dx + dz * dz;
        if (dist_sq < best_sq) {
            best_sq = dist_sq;
            best = player.entity_id;
        }
    }
    return best;
}

}  // namespace

void MobManager::spawn_passive(std::size_t count, world::World& world, double center_x,
                               double center_z) {
    for (std::size_t i = 0; i < count; ++i) {
        const double angle = rand01() * 2.0 * kPi;
        const double radius = 4.0 + rand01() * 6.0;
        const double x = center_x + std::cos(angle) * radius;
        const double z = center_z + std::sin(angle) * radius;
        const auto ground = world.surface_y(static_cast<std::int32_t>(std::floor(x)),
                                            static_cast<std::int32_t>(std::floor(z)));
        const auto type = kPassiveTypes[static_cast<std::size_t>(rand01() * 3.999)];
        (void)spawn(type, x, static_cast<double>(ground), z, static_cast<float>(rand01() * 360.0));
    }
}

std::uint32_t MobManager::spawn(std::int32_t type, double x, double y, double z, float yaw) {
    const auto species = world::mob_type(type);
    if (!species) {
        return 0;
    }
    Mob mob;
    mob.entity_id = entity::allocate_entity_id();
    mob.type = type;
    mob.pos = entity::Position{x, y, z, yaw, 0.0f};
    mob.health = species->health;
    mob.state_ticks = rand_ticks(20, 100);
    std::lock_guard<std::mutex> lock{mutex_};
    mobs_.push_back(std::move(mob));
    return mobs_.back().entity_id;
}

MobHurt MobManager::damage(std::uint32_t id, float amount, double from_x, double from_z,
                           float knockback) {
    std::lock_guard<std::mutex> lock{mutex_};
    for (std::size_t i = 0; i < mobs_.size(); ++i) {
        Mob& mob = mobs_[i];
        if (mob.entity_id != id) {
            continue;
        }
        MobHurt out;
        out.found = true;
        out.x = mob.pos.x;
        out.y = mob.pos.y;
        out.z = mob.pos.z;
        mob.health -= amount;
        if (mob.health > 0.0f) {
            out.health = mob.health;
            // EntityLiving.a(Entity, f, d0, d1)：现有动量减半后反向叠加击退，
            // 落地时向上踢 0.4（上限 0.4）
            double dx = from_x - mob.pos.x;
            double dz = from_z - mob.pos.z;
            const double len = std::sqrt(dx * dx + dz * dz);
            if (len >= 1e-4) {
                mob.velocity_x = mob.velocity_x / 2.0 - dx / len * static_cast<double>(knockback);
                mob.velocity_z = mob.velocity_z / 2.0 - dz / len * static_cast<double>(knockback);
            }
            if (mob.on_ground) {
                mob.velocity_y = mob.velocity_y / 2.0 + static_cast<double>(knockback);
                mob.velocity_y = std::min(mob.velocity_y, 0.4);
            }
            const auto species = world::mob_type(mob.type);
            if (species && !species->hostile) {
                // EntityAIPanic：逃向随机落点（findRandomTarget(5,4)），hurtTimestamp 后 100 tick
                const double angle = rand01() * 2.0 * kPi;
                const double dist = 3.0 + rand01() * 4.0;
                mob.goal_x = mob.pos.x + std::cos(angle) * dist;
                mob.goal_z = mob.pos.z + std::sin(angle) * dist;
                mob.has_goal = true;
                mob.ai = MobAi::retreat;
                mob.state_ticks = 100;
            }
            return out;
        }
        out.died = true;
        out.health = 0.0f;
        (void)remove_locked(id);
        return out;
    }
    return {};
}

bool MobManager::remove_locked(std::uint32_t id) {
    for (std::size_t i = 0; i < mobs_.size(); ++i) {
        if (mobs_[i].entity_id == id) {
            mobs_[i] = mobs_.back();
            mobs_.pop_back();
            return true;
        }
    }
    return false;
}

MobTickResult MobManager::tick(world::World& world, std::span<const PlayerSnapshot> players) {
    MobTickResult result;
    std::lock_guard<std::mutex> lock{mutex_};
    for (auto& mob : mobs_) {
        const auto species = world::mob_type(mob.type);
        if (!species) {
            continue;
        }
        if (mob.state_ticks > 0) {
            --mob.state_ticks;
        }
        if (mob.attack_cooldown > 0) {
            --mob.attack_cooldown;
        }

        // 敌对：周期性重选目标（打不到就放弃）；被动：受击才逃窜
        if (species->hostile) {
            if (mob.scan_ticks > 0) {
                --mob.scan_ticks;
            } else {
                mob.scan_ticks = kHostileScanInterval;
                const auto target =
                    nearest_hostile_target(players, mob, static_cast<double>(species->follow_range));
                if (target == 0) {
                    // EntityAITarget.unseenMemoryTicks = 60：丢视线后目标保留 60 tick
                    if (mob.target_player != 0) {
                        ++mob.unseen_ticks;
                        if (mob.unseen_ticks > 60) {
                            mob.target_player = 0;
                            mob.see_ticks = 0;
                            if (mob.bow_drawing) {
                                mob.bow_drawing = false;
                                mob.draw_ticks = 0;
                                result.draws.push_back(MobDraw{mob.entity_id, mob.pos.x, mob.pos.y,
                                                               mob.pos.z, false});
                            }
                            mob.ai = MobAi::idle;
                            mob.state_ticks = rand_ticks(20, 60);
                        }
                    }
                } else {
                    if (target != mob.target_player) {
                        mob.see_ticks = 0;
                        mob.unseen_ticks = 0;
                        if (mob.bow_drawing) {
                            mob.bow_drawing = false;
                            mob.draw_ticks = 0;
                            result.draws.push_back(MobDraw{mob.entity_id, mob.pos.x, mob.pos.y,
                                                           mob.pos.z, false});
                        }
                    }
                    mob.target_player = target;
                    mob.ai = MobAi::chase;
                }
            }
        }

        double step_x = 0.0;
        double step_z = 0.0;
        // EntityLiving.setAIMoveSpeed(speed) 同时设 landMovementFactor（travel 的摩擦参数）
        // 与 moveForward（输入）→ moveRelative 加速度 = aim×aim；地面摩擦 0.546 下
        // 终速 = aim²/(1-0.546) ≈ 2.2×aim²（骷髅 0.138 格/t，玩家疾跑 0.28 可甩开）
        const auto walk = [&](double dir_x, double dir_z, double speed_scale) {
            const double len = std::sqrt(dir_x * dir_x + dir_z * dir_z);
            if (len < 1e-6) {
                return;
            }
            const double aim = static_cast<double>(species->speed) * speed_scale;
            const double accel = aim * aim;
            step_x = dir_x / len * accel;
            step_z = dir_z / len * accel;
            mob.pos.yaw = yaw_for(dir_x, dir_z);
        };

        switch (mob.ai) {
            case MobAi::idle:
                // PathfinderGoalRandomStroll：每 tick 1/120 概率取 10 格内随机落点，
                // 不锚定生成点（mobs 会自然漂移）
                if (rand01() < 1.0 / 120.0) {
                    const double angle = rand01() * 2.0 * kPi;
                    const double dist = 3.0 + rand01() * 7.0;
                    mob.goal_x = mob.pos.x + std::cos(angle) * dist;
                    mob.goal_z = mob.pos.z + std::sin(angle) * dist;
                    mob.has_goal = true;
                    mob.ai = MobAi::wander;
                }
                break;
            case MobAi::wander: {
                const double gx = mob.goal_x - mob.pos.x;
                const double gz = mob.goal_z - mob.pos.z;
                if (!mob.has_goal || gx * gx + gz * gz < 1.0) {
                    mob.has_goal = false;
                    mob.ai = MobAi::idle;
                    break;
                }
                walk(gx, gz, static_cast<double>(species->stroll_scale));
                break;
            }
            case MobAi::retreat: {
                // EntityAIPanic：逃向随机落点，到位或 100 tick 后结束
                const double gx = mob.goal_x - mob.pos.x;
                const double gz = mob.goal_z - mob.pos.z;
                if (!mob.has_goal || gx * gx + gz * gz < 1.0 || mob.state_ticks <= 0) {
                    mob.has_goal = false;
                    mob.ai = MobAi::idle;
                    mob.state_ticks = rand_ticks(20, 100);
                    break;
                }
                walk(gx, gz, static_cast<double>(species->panic_scale));
                break;
            }
            case MobAi::chase: {
                const PlayerSnapshot* target = nullptr;
                for (const auto& player : players) {
                    if (player.entity_id == mob.target_player) {
                        target = &player;
                        break;
                    }
                }
                if (target == nullptr) {
                    mob.ai = MobAi::idle;
                    mob.target_player = 0;
                    mob.state_ticks = rand_ticks(20, 60);
                    break;
                }
                const double dx = target->x - mob.pos.x;
                const double dy = target->y - mob.pos.y;
                const double dz = target->z - mob.pos.z;
                const double dist_sq = dx * dx + dy * dy + dz * dz;
                const bool can_see = line_clear(
                    world, mob.pos.x, mob.pos.y + static_cast<double>(species->height) * 0.85,
                    mob.pos.z, target->x, target->y + 1.62, target->z);
                if (species->explodes) {
                    // PathfinderGoalSwell.e() + EntityCreeper.B_()：目标在 7 格内且可见 →
                    // state=1（引信 +1/tick），丢失/超 7 格/不可见 → state=-1（引信回退）；
                    // 引信到 30 tick 即引爆（radius 3，无火）。
                    const std::int32_t next = dist_sq <= 49.0 && can_see ? 1 : -1;
                    if (next != mob.fuse_state) {
                        mob.fuse_state = next;
                        result.ignitions.push_back(
                            MobIgnition{mob.entity_id, mob.pos.x, mob.pos.y, mob.pos.z, next});
                    }
                    mob.fuse_ticks = std::max(0, mob.fuse_ticks + next);
                    if (mob.fuse_ticks >= kFuseTicks) {
                        result.explosions.push_back(
                            MobExplosion{mob.pos.x, mob.pos.y, mob.pos.z, 3.0f});
                        MobDeath death{mob.entity_id, mob.type, mob.pos.x, mob.pos.y, mob.pos.z};
                        (void)remove_locked(mob.entity_id);
                        result.deaths.push_back(death);
                        break;
                    }
                    if (next == 1) {
                        // 引信中站定面向目标（swell goal 清空导航）
                        mob.pos.yaw = yaw_towards(mob.pos.yaw, dx, dz);
                    } else {
                        walk(dx, dz, 1.0);
                    }
                    break;
                }
                if (species->ranged) {
                    // EntityAIAttackRangedBow(skeleton, 1.0, 20, 15)：
                    //   蓄力 20 tick 才放箭（首箭也是），释放后 attackTime = 40；
                    //   seeTime ≥ 20 才停步走位（两轴 ±0.5：远 0.75 射程前进、近 0.25 后退，
                    //   每 20 tick 各 0.3 概率翻转）；射箭本身不设距离门（原版 release 只看可见性）。
                    const double horizontal_sq = dx * dx + dz * dz;
                    // EntityAIAttackRangedBow：可见性状态翻转时 seeTime 归零
                    if (can_see != (mob.see_ticks > 0)) {
                        mob.see_ticks = 0;
                    }
                    mob.see_ticks = can_see ? mob.see_ticks + 1 : mob.see_ticks - 1;
                    const bool hold_ground = horizontal_sq <= 15.0 * 15.0 && mob.see_ticks >= 20;
                    if (!hold_ground) {
                        walk(dx, dz, 1.0);
                    } else {
                        mob.pos.yaw = yaw_towards(mob.pos.yaw, dx, dz);
                        if (mob.strafe_ticks > 0) {
                            --mob.strafe_ticks;
                        } else {
                            if (rand01() < 0.3) {
                                mob.strafe_fwd = static_cast<std::int8_t>(-mob.strafe_fwd);
                            }
                            if (rand01() < 0.3) {
                                mob.strafe_side = static_cast<std::int8_t>(-mob.strafe_side);
                            }
                            mob.strafe_ticks = 20;
                        }
                        if (horizontal_sq > (15.0 * 0.75) * (15.0 * 0.75)) {
                            mob.strafe_fwd = 1;
                        } else if (horizontal_sq < (15.0 * 0.25) * (15.0 * 0.25)) {
                            mob.strafe_fwd = -1;
                        }
                        const double flat = std::sqrt(dx * dx + dz * dz);
                        if (flat >= 1e-6) {
                            // EntityMoveHelper STRAFE：aim = 0.5(倍率)×属性，输入各 0.5
                            // → 每轴加速度 = 0.25×属性（终速每轴 ≈ 0.138 格/t）
                            const double scale =
                                static_cast<double>(species->speed) * 0.25;
                            step_x = (dx / flat * static_cast<double>(mob.strafe_fwd) -
                                      dz / flat * static_cast<double>(mob.strafe_side)) * scale;
                            step_z = (dz / flat * static_cast<double>(mob.strafe_fwd) +
                                      dx / flat * static_cast<double>(mob.strafe_side)) * scale;
                        }
                    }
                    if (!mob.bow_drawing && mob.attack_cooldown <= 0) {
                        mob.bow_drawing = true;
                        mob.draw_ticks = 0;
                        result.draws.push_back(
                            MobDraw{mob.entity_id, mob.pos.x, mob.pos.y, mob.pos.z, true});
                    }
                    if (mob.bow_drawing && !can_see && mob.see_ticks < -60) {
                        // EntityAIAttackRangedBow：视线丢失 60 tick 以上收弓重瞄
                        mob.bow_drawing = false;
                        mob.draw_ticks = 0;
                        result.draws.push_back(
                            MobDraw{mob.entity_id, mob.pos.x, mob.pos.y, mob.pos.z, false});
                    }
                    if (mob.bow_drawing) {
                        ++mob.draw_ticks;
                        if (mob.draw_ticks >= 20 && can_see) {
                            mob.bow_drawing = false;
                            mob.attack_cooldown = kShootIntervalTicks;
                            result.draws.push_back(
                                MobDraw{mob.entity_id, mob.pos.x, mob.pos.y, mob.pos.z, false});
                            result.shots.push_back(
                                MobShot{mob.entity_id, target->entity_id, mob.pos.x,
                                        mob.pos.y + 1.74 /*EntitySkeletonAbstract.getHeadHeight*/,
                                        mob.pos.z, target->x, target->y + 1.8 / 3.0, target->z});
                        }
                    }
                    break;
                }
                // PathfinderGoalMeleeAttack.a()：distSq ≤ (2×width)² + 目标宽度 即可挥击，
                // 冷却 20 tick
                const double reach_sq = 4.0 * static_cast<double>(species->width) *
                                            static_cast<double>(species->width) +
                                        0.6;
                if (dist_sq > reach_sq) {
                    walk(dx, dz, 1.0);
                } else {
                    mob.pos.yaw = yaw_towards(mob.pos.yaw, dx, dz);
                    // 隔墙够不到就不挥拳（本服务端无寻路，用视线门近似原版绕路行为）
                    if (can_see && mob.attack_cooldown <= 0) {
                        mob.attack_cooldown = kAttackIntervalTicks;
                        result.attacks.push_back(MobAttack{mob.entity_id, target->entity_id,
                                                           species->attack_damage, mob.pos.x,
                                                           mob.pos.z});
                    }
                }
                break;
            }
        }

        // 物理：vanilla travel——moveRelative 加速度 → move → 摩擦（f6 取 move 之前的
        // 落地状态：地面 slipperiness 0.6×0.91 = 0.546，空中 0.91）；撞墙只清被挡轴。
        mob.velocity_x += step_x;
        mob.velocity_z += step_z;
        mob.velocity_y = std::max(world::kEntityTerminalY, mob.velocity_y + world::kEntityGravity);
        double dx = mob.velocity_x;
        double dy = mob.velocity_y;
        double dz = mob.velocity_z;
        const bool was_on_ground = mob.on_ground;
        auto box = world::entity_box(mob.pos.x, mob.pos.y, mob.pos.z, species->width, species->height);
        const auto before = mob.pos;
        const auto outcome = world::move_with_collision(world, box, dx, dy, dz, 1.0);
        mob.pos.x = (box.min_x + box.max_x) * 0.5;
        mob.pos.y = box.min_y;
        mob.pos.z = (box.min_z + box.max_z) * 0.5;
        mob.on_ground = outcome.on_ground;
        if (outcome.on_ground) {
            mob.velocity_y = 0.0;
        }
        const double friction = was_on_ground ? 0.546 : 0.91;
        mob.velocity_x = outcome.blocked_x ? 0.0 : mob.velocity_x * friction;
        mob.velocity_z = outcome.blocked_z ? 0.0 : mob.velocity_z * friction;
        // 漫游/逃窜撞墙：放弃当前落点（vanilla 会重新选点）
        if ((outcome.blocked_x || outcome.blocked_z) &&
            (mob.ai == MobAi::wander || mob.ai == MobAi::retreat)) {
            mob.has_goal = false;
            mob.ai = MobAi::idle;
        }
        if (std::abs(mob.velocity_x) < 0.001) {
            mob.velocity_x = 0.0;
        }
        if (std::abs(mob.velocity_z) < 0.001) {
            mob.velocity_z = 0.0;
        }

        const double moved_sq = (mob.pos.x - before.x) * (mob.pos.x - before.x) +
                                (mob.pos.y - before.y) * (mob.pos.y - before.y) +
                                (mob.pos.z - before.z) * (mob.pos.z - before.z);
        if (moved_sq > 1e-8) {
            result.moved.push_back(MobMove{mob.entity_id, mob.pos.x, mob.pos.y, mob.pos.z,
                                           mob.pos.yaw});
        }
    }
    return result;
}

}  // namespace cyane::net
