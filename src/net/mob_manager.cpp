#include "cyane/net/mob_manager.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <random>

#include "cyane/item/item_tools.hpp"
#include "cyane/world/mob_types.hpp"
#include "cyane/world/pathfinding.hpp"

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

// EntityLivingBase.knockBack(0.4)：现有水平动量减半后反向叠加；落地时竖直动量
// 减半 +0.4（上限 0.4）。方向 = 攻击者 − 自己，距离过近（<1e-4）时随机抖动定向。
void apply_knockback(Mob& mob, double from_x, double from_z, float knockback) {
    double dx = from_x - mob.pos.x;
    double dz = from_z - mob.pos.z;
    while (dx * dx + dz * dz < 1.0e-4) {
        dx = (rand01() - rand01()) * 0.01;
        dz = (rand01() - rand01()) * 0.01;
    }
    const double len = std::sqrt(dx * dx + dz * dz);
    mob.velocity_x = mob.velocity_x / 2.0 - dx / len * static_cast<double>(knockback);
    mob.velocity_z = mob.velocity_z / 2.0 - dz / len * static_cast<double>(knockback);
    if (mob.on_ground) {
        mob.velocity_y = mob.velocity_y / 2.0 + static_cast<double>(knockback);
        mob.velocity_y = std::min(mob.velocity_y, 0.4);
    }
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
        // attackEntityFrom 的无敌窗分支：hurtResistantTime > 10 时仅更高伤害可破防
        // 且只结算差值；否则全额结算并重置窗口（maxHurtResistantTime = 20）
        if (mob.hurt_resistant_ticks > 10) {
            if (amount <= mob.last_damage) {
                return MobHurt{};  // 吸收：无音效无伤害（vanilla return false）
            }
            const float net = amount - mob.last_damage;
            mob.last_damage = amount;
            const auto species2 = world::mob_type(mob.type);
            const float armored = species2
                ? item::damage_after_armor(net, species2->armor_points, 0.0f)
                : net;
            mob.health -= armored;
            if (mob.health <= 0.0f) {
                out.died = true;
                out.health = 0.0f;
                mob.death_timer = 20;
                return out;
            }
            out.health = mob.health;
            // 破防伤害也走击退（vanilla damageEntity 路径同样触发 knockBack）
            apply_knockback(mob, from_x, from_z, knockback);
            const auto species = world::mob_type(mob.type);
            if (species && !species->hostile) {
                mob.ai = MobAi::retreat;
                mob.state_ticks = 100;
            }
            return out;
        }
        mob.last_damage = amount;
        mob.hurt_resistant_ticks = 20;
        {
            const auto species2 = world::mob_type(mob.type);
            amount = species2
                ? item::damage_after_armor(amount, species2->armor_points, 0.0f)
                : amount;
        }
        mob.health -= amount;
        if (mob.health > 0.0f) {
            out.health = mob.health;
            apply_knockback(mob, from_x, from_z, knockback);
            const auto species = world::mob_type(mob.type);
            if (species && !species->hostile) {
                // EntityAIPanic：hurtTimestamp 后 100 tick；落点与寻路在下一 tick 的
                // retreat 分支做（damage 不持 World 引用）
                mob.ai = MobAi::retreat;
                mob.state_ticks = 100;
            }
            return out;
        }
        out.died = true;
        out.health = 0.0f;
        // 原版 onDeath：立即掉落 + EntityStatus 3 + deathTime 20 tick 后才 setDead
        mob.death_timer = 20;
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

bool MobManager::interact(std::uint32_t id, std::int16_t held_item_id,
                           std::int16_t held_item_damage) {
    std::lock_guard<std::mutex> lock{mutex_};
    for (auto& mob : mobs_) {
        if (mob.entity_id != id) {
            continue;
        }
        const auto species = world::mob_type(mob.type);
        if (!species) {
            return false;
        }
        // 剪羊毛：手持 shears (359) 右键 sheep (91)
        if (mob.type == 91 && !mob.sheared && held_item_id == 359) {
            mob.sheared = true;
            return true;
        }
        // 挤奶：手持 bucket (325) 右键 cow (92)
        if (mob.type == 92 && held_item_id == 325) {
            return true;
        }
        // 喂食繁殖
        // 牛/羊：wheat (296)；猪：carrot (391) / potato (392)；鸡：seeds (291)
        bool is_breeding_food = false;
        if (mob.type == 92 || mob.type == 91) {  // cow / sheep
            is_breeding_food = (held_item_id == 296);  // wheat
        } else if (mob.type == 90) {  // pig
            is_breeding_food = (held_item_id == 391 || held_item_id == 392);  // carrot / potato
        } else if (mob.type == 93) {  // chicken
            is_breeding_food = (held_item_id == 291);  // seeds
        }
        if (is_breeding_food) {
            mob.in_love = true;
            mob.love_timer = 2000;  // 100 秒爱心模式
            return true;
        }
        return false;
    }
    return false;
}

MobTickResult MobManager::tick(world::World& world, std::span<const PlayerSnapshot> players,
                               bool daytime) {
    MobTickResult result;
    std::lock_guard<std::mutex> lock{mutex_};
    std::vector<std::uint32_t> to_remove;
    for (auto& mob : mobs_) {
        const auto species = world::mob_type(mob.type);
        if (!species) {
            continue;
        }
        // 液体检测（travel 的 water/lava 分支开关；判脚下格）
        const auto feet_block_id = world::block_id(world.block_at(
            static_cast<std::int32_t>(std::floor(mob.pos.x)),
            static_cast<std::int32_t>(std::floor(mob.pos.y)),
            static_cast<std::int32_t>(std::floor(mob.pos.z))));
        const bool in_water = feet_block_id == 8 || feet_block_id == 9;
        const bool in_lava = feet_block_id == 10 || feet_block_id == 11;

        // ---- 死亡倒计时（onDeath → deathTime 20 tick → setDead） ----
        if (mob.death_timer >= 0) {
            if (mob.death_timer == 20) {
                // 死亡瞬间：EntityStatus 3 + 死亡音 + 掉落（server 端处理，不销毁实体）
                result.deaths.push_back(
                    MobDeath{mob.entity_id, mob.type, mob.pos.x, mob.pos.y, mob.pos.z, false});
            }
            --mob.death_timer;
            if (mob.death_timer < 0) {
                // 动画结束：真正销毁
                result.deaths.push_back(
                    MobDeath{mob.entity_id, mob.type, mob.pos.x, mob.pos.y, mob.pos.z, true});
                to_remove.push_back(mob.entity_id);
            }
            continue;  // 死亡中的生物不跑 AI/物理
        }

        // ---- 环境音（onEntityUpdate 的 livingSoundTime） ----
        --mob.living_sound_timer;
        if (mob.living_sound_timer <= 0) {
            if (species->ambient_sound != 0) {
                result.sounds.push_back(MobSound{mob.entity_id, species->ambient_sound,
                                                 mob.pos.x, mob.pos.y, mob.pos.z});
            }
            mob.living_sound_timer = species->talk_interval;
        }

        // ---- 落差伤害（fall(distance)：ceil(落差-3)，落地瞬间结算） ----
        if (in_water || in_lava) {
            mob.fall_start_y = mob.pos.y;  // vanilla：进液体清 fallDistance
        } else if (mob.was_on_ground_last_tick && !mob.on_ground) {
            mob.fall_start_y = mob.pos.y;  // 刚离地
        } else if (!mob.was_on_ground_last_tick && mob.on_ground) {
            const double fall = mob.fall_start_y - mob.pos.y;
            if (fall > 3.0) {
                mob.health -= static_cast<float>(std::ceil(fall - 3.0));
                result.sounds.push_back(MobSound{mob.entity_id, species->hurt_sound, mob.pos.x,
                                                 mob.pos.y, mob.pos.z});
                if (mob.health <= 0.0f) {
                    mob.death_timer = 20;
                    // 设置经验值
                    int experience = 0;
                    if (species) {
                        if (species->hostile) {
                            experience = species->xp_value; // 敌对生物固定 5 点经验
                        } else {
                            // 被动生物：1 + rand(3) 点经验
                            static thread_local std::mt19937 passive_rand{std::random_device{}()};
                            experience = 1 + std::uniform_int_distribution<int>(0, 2)(passive_rand);
                        }
                    }
                    result.deaths.push_back(MobDeath{mob.entity_id, mob.type, mob.pos.x,
                                                     mob.pos.y, mob.pos.z, false, experience});
                    continue;
                }
            }
            mob.fall_start_y = 0.0;
        }
        mob.was_on_ground_last_tick = mob.on_ground;

        // ---- 火焰/岩浆伤害（setOnFireFromLava 4/t、火 1/t） ----
        const auto feet_block = world.block_at(
            static_cast<std::int32_t>(std::floor(mob.pos.x)),
            static_cast<std::int32_t>(std::floor(mob.pos.y)),
            static_cast<std::int32_t>(std::floor(mob.pos.z)));
        const auto feet_id = world::block_id(feet_block);
        float env_damage = 0.0f;
        if (feet_id == 51) {
            env_damage = 1.0f;  // 火
        } else if (feet_id == 10 || feet_id == 11) {
            env_damage = 4.0f;  // 岩浆
        }
        if (env_damage > 0.0f) {
            mob.health -= env_damage;
            result.sounds.push_back(MobSound{mob.entity_id, species->hurt_sound, mob.pos.x,
                                             mob.pos.y, mob.pos.z});
            if (mob.health <= 0.0f) {
                mob.death_timer = 20;
                // 设置经验值
                int experience = 0;
                if (species) {
                    if (species->hostile) {
                        experience = species->xp_value; // 敌对生物固定 5 点经验
                    } else {
                        // 被动生物：1 + rand(3) 点经验
                        static thread_local std::mt19937 passive_rand{std::random_device{}()};
                        experience = 1 + std::uniform_int_distribution<int>(0, 2)(passive_rand);
                    }
                }
                result.deaths.push_back(MobDeath{mob.entity_id, mob.type, mob.pos.x, mob.pos.y,
                                                 mob.pos.z, false, experience});
                continue;
            }
        }

        // ---- 日光燃烧（僵尸/骷髅：白天 + 露天 → setFire(8)，持续烧 1 血/秒） ----
        if (species->burns_in_daylight && daytime && mob.idle_ticks % 20 == 0) {
            // canSeeSky 近似：头顶到世界顶无实心方块
            bool sky_clear = true;
            for (auto sy = static_cast<std::int32_t>(std::floor(mob.pos.y)) + 2; sy < 256; ++sy) {
                if (world::is_solid(world.block_at(
                        static_cast<std::int32_t>(std::floor(mob.pos.x)), sy,
                        static_cast<std::int32_t>(std::floor(mob.pos.z))))) {
                    sky_clear = false;
                    break;
                }
            }
            if (sky_clear && feet_id != 10 && feet_id != 11) {  // 水中/岩浆中不烧
                mob.health -= 1.0f;
                result.sounds.push_back(MobSound{mob.entity_id, species->hurt_sound, mob.pos.x,
                                                 mob.pos.y, mob.pos.z});
                if (mob.health <= 0.0f) {
                    mob.death_timer = 20;
                    // 设置经验值
                    int experience = 0;
                    if (species) {
                        if (species->hostile) {
                            experience = species->xp_value; // 敌对生物固定 5 点经验
                        } else {
                            // 被动生物：1 + rand(3) 点经验
                            static thread_local std::mt19937 passive_rand{std::random_device{}()};
                            experience = 1 + std::uniform_int_distribution<int>(0, 2)(passive_rand);
                        }
                    }
                    result.deaths.push_back(MobDeath{mob.entity_id, mob.type, mob.pos.x,
                                                     mob.pos.y, mob.pos.z, false, experience});
                    continue;
                }
            }
        }

        // ---- Despawn（despawnEntity：>128 格立即消失；>32 格且 idle>600 时 1/800）。
        // vanilla getClosestPlayerToEntity 无玩家时返回 null → 整个 despawn 逻辑跳过 ----
        if (!players.empty()) {
        double nearest_sq = 1e18;
        for (const auto& p : players) {
            const double dx = p.x - mob.pos.x;
            const double dy = p.y - mob.pos.y;
            const double dz = p.z - mob.pos.z;
            const double d2 = dx * dx + dy * dy + dz * dz;
            if (d2 < nearest_sq) {
                nearest_sq = d2;
            }
        }
        if (nearest_sq > 128.0 * 128.0) {
            result.deaths.push_back(
                MobDeath{mob.entity_id, mob.type, mob.pos.x, mob.pos.y, mob.pos.z, true});
            to_remove.push_back(mob.entity_id);
            continue;
        }
        if (nearest_sq > 32.0 * 32.0) {
            ++mob.idle_ticks;
            if (mob.idle_ticks > 600 && rand01() < 1.0 / 800.0) {
                result.deaths.push_back(
                    MobDeath{mob.entity_id, mob.type, mob.pos.x, mob.pos.y, mob.pos.z, true});
                to_remove.push_back(mob.entity_id);
                continue;
            }
        } else {
            mob.idle_ticks = 0;
        }
        }  // players.empty() 门控结束

        if (mob.state_ticks > 0) {
            --mob.state_ticks;
        }
        if (mob.attack_cooldown > 0) {
            --mob.attack_cooldown;
        }
        if (mob.hurt_resistant_ticks > 0) {
            --mob.hurt_resistant_ticks;
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
        // EntityLivingBase.travel 的地面/空中分流（击退时"不再走位"的机制所在）：
        //   f6 摩擦 = onGround ? 脚下方块 slipperiness×0.91 : 0.91（用 move 前的落地状态）
        //   f7 = 0.16277136/f6³（地面 ≈1.0）
        //   f8 = onGround ? getAIMoveSpeed()×f7 : jumpMovementFactor(0.02)
        //   moveRelative 加速度 = moveForward × f8 → 空中 AI 控制力几乎为零（骷髅 0.005），
        //   被击飞的生物只沿击退动量飞行，落地后才恢复走位
        const bool was_on_ground = mob.on_ground;
        const bool ground_accel = was_on_ground && !in_water && !in_lava;
        const double friction =
            was_on_ground && !in_water && !in_lava
                ? world::block_slipperiness(world.block_at(
                      static_cast<std::int32_t>(std::floor(mob.pos.x)),
                      static_cast<std::int32_t>(std::floor(mob.pos.y)) - 1,
                      static_cast<std::int32_t>(std::floor(mob.pos.z)))) *
                      0.91
                : 0.91;
        const double ground_accel_factor = 0.16277136 / (friction * friction * friction);
        const auto walk = [&](double dir_x, double dir_z, double speed_scale) {
            const double len = std::sqrt(dir_x * dir_x + dir_z * dir_z);
            if (len < 1e-6) {
                return;
            }
            const double aim = static_cast<double>(species->speed) * speed_scale;
            const double f8 = ground_accel ? aim * ground_accel_factor : 0.02;
            const double accel = aim * f8;  // moveRelative：input(moveForward) × f8
            step_x = dir_x / len * accel;
            step_z = dir_z / len * accel;
        };

        // PathNavigateGround.onUpdateNavigation：pathFollow（节点推进 + 直线捷径 +
        // 卡住检测）→ 对当前节点 setMoveTo（加速度朝节点中心）
        const auto follow_path = [&](double speed_scale) {
            if (!mob.has_path && mob.path.empty()) {
                return;
            }
            const double max_dist = static_cast<double>(species->width) > 0.75
                                        ? static_cast<double>(species->width) / 2.0
                                        : 0.75 - static_cast<double>(species->width) / 2.0;
            const double pathable_y = std::floor(mob.pos.y + 0.5);
            // pathFollow：先按 Y 层截断可直视的最大下标
            std::size_t direct_limit = mob.path.size();
            for (std::size_t j = mob.path_index; j < mob.path.size(); ++j) {
                if (static_cast<double>(mob.path[j].y) != pathable_y) {
                    direct_limit = j;
                    break;
                }
            }
            if (mob.path_index < mob.path.size()) {
                const auto& current = mob.path[mob.path_index];
                if (std::abs(mob.pos.x - (static_cast<double>(current.x) + 0.5)) < max_dist &&
                    std::abs(mob.pos.z - (static_cast<double>(current.z) + 0.5)) < max_dist &&
                    std::abs(mob.pos.y - static_cast<double>(current.y)) < 1.0) {
                    ++mob.path_index;
                }
            }
            // 直线捷径：从最远可直视节点倒着找（isDirectPathBetweenPoints）
            const auto shape = world::mob_shape(species->width, species->height);
            const world::Path::Vec from{mob.pos.x, pathable_y, mob.pos.z};
            for (std::size_t j = direct_limit; j > mob.path_index;) {
                --j;
                const auto& node = mob.path[j];
                const double offset =
                    static_cast<double>(static_cast<std::int32_t>(species->width + 1.0f)) * 0.5;
                const world::Path::Vec to{static_cast<double>(node.x) + offset,
                                          static_cast<double>(node.y),
                                          static_cast<double>(node.z) + offset};
                if (world::is_direct_path_between_points(world, shape, from, to)) {
                    if (j > mob.path_index) {
                        mob.path_index = j;
                    }
                    break;
                }
            }
            // checkForStuck：每 100 tick 检查位移 <1.5 → 清路径
            ++mob.stuck_ticks;
            if (mob.stuck_ticks >= 100) {
                const double moved = (mob.pos.x - mob.stuck_x) * (mob.pos.x - mob.stuck_x) +
                                     (mob.pos.z - mob.stuck_z) * (mob.pos.z - mob.stuck_z);
                if (moved < 2.25) {
                    mob.has_path = false;
                    mob.path.clear();
                }
                mob.stuck_ticks = 0;
                mob.stuck_x = mob.pos.x;
                mob.stuck_z = mob.pos.z;
            }
            if (mob.path_index >= mob.path.size()) {
                mob.has_path = false;
                mob.path.clear();
                return;
            }
            const auto& node = mob.path[mob.path_index];
            // getVectorFromIndex：节点中心偏移 (int)(width+1)*0.5
            const double offset = static_cast<double>(static_cast<std::int32_t>(species->width + 1.0f)) * 0.5;
            const double tx = static_cast<double>(node.x) + offset;
            const double tz = static_cast<double>(node.z) + offset;
            const double dxn = tx - mob.pos.x;
            const double dzn = tz - mob.pos.z;
            if (dxn * dxn + dzn * dzn > 1e-6) {
                walk(dxn, dzn, speed_scale);
                mob.pos.yaw = yaw_towards(mob.pos.yaw, dxn, dzn);
            }
        };

        switch (mob.ai) {
            case MobAi::idle:
                // PathfinderGoalLookAtPlayer(8.0F) / RandomLookaround：闲置时看向 8 格内
                // 最近玩家（faceEntity 限速），否则每 tick 1/50 概率随机转头
                {
                    const PlayerSnapshot* look_target = nullptr;
                    double look_sq = 8.0 * 8.0;
                    for (const auto& p : players) {
                        const double dx2 = p.x - mob.pos.x;
                        const double dz2 = p.z - mob.pos.z;
                        const double dy2 = p.y - mob.pos.y;
                        const double d2 = dx2 * dx2 + dy2 * dy2 + dz2 * dz2;
                        if (d2 < look_sq) {
                            look_sq = d2;
                            look_target = &p;
                        }
                    }
                    if (look_target != nullptr) {
                        mob.pos.yaw = yaw_towards(mob.pos.yaw, look_target->x - mob.pos.x,
                                                  look_target->z - mob.pos.z);
                    } else if (rand01() < 1.0 / 50.0) {
                        mob.pos.yaw = static_cast<float>(rand01() * 360.0);
                    }
                }
                // PathfinderGoalRandomStroll：每 tick 1/120 概率经 RandomPositionGenerator
                // 找 10×7 内随机落点（canEntityStandOnPos 过滤），寻路前往
                if (rand01() < 1.0 / 120.0) {
                    if (const auto goal = world::random_position(
                            world, mob.pos.x, mob.pos.y, mob.pos.z, 10, 7, 0.0, 0.0)) {
                        // tryMoveToXYZ → getPathToPos：落点先归一到能站的方块
                        const auto spot = world::path_target_block(world, goal->x, goal->y, goal->z);
                        auto path = world::find_path(
                            world, mob.pos.x, mob.pos.y, mob.pos.z, mob.on_ground,
                            species->width, species->height,
                            static_cast<double>(spot.x) + 0.5, static_cast<double>(spot.y) + 0.5,
                            static_cast<double>(spot.z) + 0.5,
                            static_cast<float>(species->follow_range));
                        if (path) {
                            mob.path = std::move(path->points);
                            mob.path_index = 0;
                            mob.has_path = !mob.path.empty();
                            mob.ai = MobAi::wander;
                        }
                    }
                }
                break;
            case MobAi::wander:
                follow_path(static_cast<double>(species->stroll_scale));
                if (!mob.has_path) {
                    mob.ai = MobAi::idle;
                }
                break;
            case MobAi::retreat:
                // EntityAIPanic：逃向随机落点（寻路）。找不到落点或无路径时原版每 tick
                // 重新 shouldExecute（findRandomPosition + tryMoveToXYZ），因此这里同样重试，
                // 直到受击记忆（vengeanceTime = 100 tick）结束
                if (!mob.has_path && mob.state_ticks > 0) {
                    if (const auto goal = world::random_position(
                            world, mob.pos.x, mob.pos.y, mob.pos.z, 5, 4, 0.0, 0.0)) {
                        const auto spot = world::path_target_block(world, goal->x, goal->y, goal->z);
                        auto path = world::find_path(
                            world, mob.pos.x, mob.pos.y, mob.pos.z, mob.on_ground,
                            species->width, species->height,
                            static_cast<double>(spot.x) + 0.5, static_cast<double>(spot.y) + 0.5,
                            static_cast<double>(spot.z) + 0.5,
                            static_cast<float>(species->follow_range));
                        if (path) {
                            mob.path = std::move(path->points);
                            mob.path_index = 0;
                            mob.has_path = !mob.path.empty();
                        }
                    }
                }
                follow_path(static_cast<double>(species->panic_scale));
                if (mob.state_ticks <= 0) {
                    mob.ai = MobAi::idle;
                    mob.state_ticks = rand_ticks(20, 100);
                }
                break;
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
                        // 引爆即消失：不播死亡动画/不掉自身战利品，只销毁实体
                        result.deaths.push_back(MobDeath{mob.entity_id, mob.type, mob.pos.x,
                                                         mob.pos.y, mob.pos.z, true});
                        (void)remove_locked(mob.entity_id);
                        break;
                    }
                    if (next == 1) {
                        // 引信中站定面向目标（swell goal 清空导航）
                        mob.pos.yaw = yaw_towards(mob.pos.yaw, dx, dz);
                        mob.has_path = false;
                        mob.path.clear();
                    } else {
                        // 追向目标：导航到实体（MeleeAttack 节奏重寻路）
                        if (mob.repath_ticks > 0) {
                            --mob.repath_ticks;
                        } else {
                            mob.repath_ticks = 4 + rand_ticks(0, 7);
                            auto path = world::find_path(
                                world, mob.pos.x, mob.pos.y, mob.pos.z, mob.on_ground,
                                species->width, species->height, target->x, target->y, target->z,
                                static_cast<float>(species->follow_range));
                            if (path) {
                                mob.path = std::move(path->points);
                                mob.path_index = 0;
                                mob.has_path = !mob.path.empty();
                            }
                        }
                        follow_path(1.0);
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
                        // BowShoot：超出射程或未确认可见 → 导航接近（每 10 tick 重寻路）
                        if (mob.repath_ticks > 0) {
                            --mob.repath_ticks;
                        } else {
                            mob.repath_ticks = 10;
                            auto path = world::find_path(
                                world, mob.pos.x, mob.pos.y, mob.pos.z, mob.on_ground,
                                species->width, species->height, target->x, target->y, target->z,
                                static_cast<float>(species->follow_range));
                            if (path) {
                                mob.path = std::move(path->points);
                                mob.path_index = 0;
                                mob.has_path = !mob.path.empty();
                            }
                        }
                        follow_path(1.0);
                    } else {
                        // 停步走位（导航清空）
                        mob.has_path = false;
                        mob.path.clear();
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
                            // EntityMoveHelper.onUpdateMoveHelper STRAFE 分支（逐行转写）：
                            // speed=0.25×属性；moveForward/moveStrafing 输入各 ±0.5 →
                            // 每轴加速度 = 0.5 × 0.25×属性 = 0.125×属性（终速每轴 ≈ 0.069 格/t）
                            const double attr = static_cast<double>(species->speed);
                            const double aim = 0.25 * attr;  // landMovementFactor
                            const double fwd = static_cast<double>(mob.strafe_fwd);
                            const double side = static_cast<double>(mob.strafe_side);
                            // 迈步探查：按朝向旋转后的位移方向（f7/f8）落在的格子
                            // 必须是 WALKABLE，否则改为沿朝向全速前进（moveForward=1.0、
                            // landMovementFactor=属性）——vanilla 的走位崖边保护
                            const double probe_x =
                                (dx / flat * fwd - dz / flat * side) * 0.5 * aim;
                            const double probe_z =
                                (dz / flat * fwd + dx / flat * side) * 0.5 * aim;
                            const auto probe_type = world::path_node_type_single(
                                world, static_cast<std::int32_t>(std::floor(mob.pos.x + probe_x)),
                                static_cast<std::int32_t>(std::floor(mob.pos.y)),
                                static_cast<std::int32_t>(std::floor(mob.pos.z + probe_z)));
                            // 空中加速度 = moveForward × jumpMovementFactor（0.02）
                            if (probe_type != world::PathNodeType::walkable) {
                                const double rad = static_cast<double>(mob.pos.yaw) * kPi / 180.0;
                                const double f8 = ground_accel ? attr * ground_accel_factor : 0.02;
                                step_x = -std::sin(rad) * f8;
                                step_z = std::cos(rad) * f8;
                            } else {
                                const double f8 = ground_accel ? aim * ground_accel_factor : 0.02;
                                const double scale = 0.5 * f8;
                                step_x = (dx / flat * fwd - dz / flat * side) * scale;
                                step_z = (dz / flat * fwd + dx / flat * side) * scale;
                            }
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
                    // PathfinderGoalMeleeAttack：4 + rand(7) tick 重寻路
                    if (mob.repath_ticks > 0) {
                        --mob.repath_ticks;
                    } else {
                        mob.repath_ticks = 4 + rand_ticks(0, 7);
                        auto path = world::find_path(
                            world, mob.pos.x, mob.pos.y, mob.pos.z, mob.on_ground,
                            species->width, species->height, target->x, target->y, target->z,
                            static_cast<float>(species->follow_range));
                        if (path) {
                            mob.path = std::move(path->points);
                            mob.path_index = 0;
                            mob.has_path = !mob.path.empty();
                        }
                    }
                    follow_path(1.0);
                } else {
                    mob.pos.yaw = yaw_towards(mob.pos.yaw, dx, dz);
                    // 攻击距离内：视线可见才挥拳（无寻路时可停滞）
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

        // 物理：vanilla travel——moveRelative 加速度 → move → 摩擦；撞墙只清被挡轴。
        // 水分支（travel water）：加速度摩擦 0.02 → move → 三轴 ×0.8 阻尼 → −0.02 下沉
        // → 碰壁上浮 0.3；岩浆同构但阻尼 0.5。液体中不套用地面/空气摩擦与 −0.08 重力。
        mob.velocity_x += step_x;
        mob.velocity_z += step_z;
        if (in_water || in_lava) {
            mob.velocity_y = std::max(world::kEntityTerminalY, mob.velocity_y);
        } else {
            mob.velocity_y = std::max(world::kEntityTerminalY, mob.velocity_y + world::kEntityGravity);
        }
        double dx = mob.velocity_x;
        double dy = mob.velocity_y;
        double dz = mob.velocity_z;
        auto box = world::entity_box(mob.pos.x, mob.pos.y, mob.pos.z, species->width, species->height);
        const auto before = mob.pos;
        const auto outcome = world::move_with_collision(world, box, dx, dy, dz, 1.0);
        mob.pos.x = (box.min_x + box.max_x) * 0.5;
        mob.pos.y = box.min_y;
        mob.pos.z = (box.min_z + box.max_z) * 0.5;
        mob.on_ground = outcome.on_ground;
        if (in_water || in_lava) {
            const double damping = in_water ? 0.800000011920929 : 0.5;
            mob.velocity_x = outcome.blocked_x ? 0.0 : mob.velocity_x * damping;
            mob.velocity_z = outcome.blocked_z ? 0.0 : mob.velocity_z * damping;
            mob.velocity_y *= damping;
            mob.velocity_y -= 0.02;
            if ((outcome.blocked_x || outcome.blocked_z) && mob.velocity_y < 0.3) {
                mob.velocity_y = 0.3;  // 碰壁上浮（vanilla isOffsetPositionInLiquid 检查的简化）
            }
        } else {
            if (outcome.on_ground) {
                mob.velocity_y = 0.0;
            }
            mob.velocity_x = outcome.blocked_x ? 0.0 : mob.velocity_x * friction;
            mob.velocity_z = outcome.blocked_z ? 0.0 : mob.velocity_z * friction;
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

    // 移除本拍标记的生物（死亡动画结束 / despawn）
    if (!to_remove.empty()) {
        mobs_.erase(std::remove_if(mobs_.begin(), mobs_.end(),
                                   [&](const Mob& m) {
                                       return std::find(to_remove.begin(), to_remove.end(),
                                                        m.entity_id) != to_remove.end();
                                   }),
                    mobs_.end());
    }

    // EntityLivingBase.collideWithNearbyEntities → Entity.applyEntityCollision：
    // 包围盒相交即互推。d0/d1 = 对方−自己，按 absMax 归一（vanilla 对 max 分量开方），
    // 强度 0.05×(1/d2 ≤1)，双方各反向叠加——vanilla 里每个实体各自跑一遍，
    // 故这里全对全双循环（等效双倍强度）。
    for (std::size_t i = 0; i < mobs_.size(); ++i) {
        Mob& self = mobs_[i];
        const auto self_species = world::mob_type(self.type);
        if (!self_species) {
            continue;
        }
        const auto self_box = world::entity_box(self.pos.x, self.pos.y, self.pos.z,
                                                self_species->width, self_species->height);
        for (std::size_t j = 0; j < mobs_.size(); ++j) {
            if (j == i) {
                continue;
            }
            Mob& other = mobs_[j];
            const auto other_species = world::mob_type(other.type);
            if (!other_species) {
                continue;
            }
            const auto other_box = world::entity_box(other.pos.x, other.pos.y, other.pos.z,
                                                     other_species->width, other_species->height);
            if (self_box.max_x <= other_box.min_x || self_box.min_x >= other_box.max_x ||
                self_box.max_y <= other_box.min_y || self_box.min_y >= other_box.max_y ||
                self_box.max_z <= other_box.min_z || self_box.min_z >= other_box.max_z) {
                continue;
            }
            double d0 = other.pos.x - self.pos.x;
            double d1 = other.pos.z - self.pos.z;
            double d2 = std::max(std::abs(d0), std::abs(d1));
            if (d2 < 0.009999999776482582) {
                continue;
            }
            d2 = std::sqrt(d2);
            d0 /= d2;
            d1 /= d2;
            double d3 = std::min(1.0 / d2, 1.0);
            d0 *= d3 * 0.05000000074505806;
            d1 *= d3 * 0.05000000074505806;
            self.velocity_x -= d0;
            self.velocity_z -= d1;
            other.velocity_x += d0;
            other.velocity_z += d1;
        }
    }
    return result;
}

}  // namespace cyane::net
