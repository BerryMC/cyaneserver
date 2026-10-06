#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <string_view>
#include <vector>

#include "cyane/net/item_drop.hpp"
#include "cyane/net/mob_manager.hpp"
#include "cyane/net/projectile_manager.hpp"
#include "cyane/net/packet_writers.hpp"
#include "cyane/world/blocks.hpp"
#include "cyane/world/mob_types.hpp"
#include "cyane/world/physics.hpp"
#include "cyane/world/world.hpp"
#include "test_framework.hpp"

using namespace cyane;

namespace {

// 超平坦基线：y0 基岩、y3 草方块（World 未物化区块的 baseline），实体脚底落在 y=4
constexpr std::int32_t kGroundY = 4;

[[nodiscard]] net::PlayerSnapshot player_at(std::uint32_t id, double x, double y, double z) {
    net::PlayerSnapshot snap;
    snap.entity_id = id;
    snap.x = x;
    snap.y = y;
    snap.z = z;
    return snap;
}

}  // namespace

// 物种表：类型 ↔ 存档 NBT 名 双向一致，关键属性与 vanilla 提取值吻合（R-022）
CYANE_TEST(mob_species_table_matches_vanilla) {
    for (const auto type : {50, 51, 52, 54, 90, 91, 92, 93}) {
        const auto species = world::mob_type(type);
        CYANE_CHECK(species.has_value());
        if (!species) {
            continue;
        }
        CYANE_CHECK_EQ(species->type, type);
        const auto back = world::mob_type_from_nbt(species->nbt_id);
        CYANE_CHECK(back.has_value() && *back == type);
        CYANE_CHECK(species->width > 0.0f && species->height > 0.0f && species->health > 0.0f);
    }
    const auto zombie = world::mob_type(54);
    CYANE_CHECK(zombie->hostile && !zombie->ranged);
    CYANE_CHECK_EQ(zombie->health, 20.0f);
    CYANE_CHECK_EQ(zombie->attack_damage, 3.0f);
    CYANE_CHECK_EQ(zombie->width, 0.6f);
    CYANE_CHECK_EQ(zombie->height, 1.95f);

    const auto chicken = world::mob_type(93);
    CYANE_CHECK_EQ(chicken->health, 4.0f);
    CYANE_CHECK_EQ(chicken->width, 0.4f);
    CYANE_CHECK(!chicken->hostile);

    const auto creeper = world::mob_type(50);
    const auto skeleton = world::mob_type(51);
    const auto spider = world::mob_type(52);
    CYANE_CHECK(creeper->explodes && !creeper->ranged);
    CYANE_CHECK(skeleton->ranged && skeleton->hostile);
    CYANE_CHECK_EQ(spider->health, 16.0f);
    CYANE_CHECK_EQ(spider->width, 1.4f);

    CYANE_CHECK(!world::mob_type(1).has_value());     // 物品实体不是生物
    CYANE_CHECK(!world::mob_type(999).has_value());
    CYANE_CHECK(!world::mob_type_from_nbt("minecraft:enderman").has_value());
}

// 刷怪蛋：item 383 的 damage 即实体类型 id；其他物品/未知类型不生成
CYANE_TEST(spawn_egg_maps_damage_to_entity_type) {
    CYANE_CHECK_EQ(world::spawn_egg_type(383, 54).value_or(0), 54);   // 僵尸
    CYANE_CHECK_EQ(world::spawn_egg_type(383, 50).value_or(0), 50);   // 苦力怕
    CYANE_CHECK_EQ(world::spawn_egg_type(383, 93).value_or(0), 93);   // 鸡
    CYANE_CHECK(!world::spawn_egg_type(383, 7).has_value());          // 未建模实体
    CYANE_CHECK(!world::spawn_egg_type(260, 54).has_value());         // 苹果不是蛋
    CYANE_CHECK(!world::spawn_egg_type(383, 0).has_value());
}

// 碰撞判定：固体/非固体
CYANE_TEST(collision_solidity_table) {
    CYANE_CHECK(world::is_solid(world::kStateStone));
    CYANE_CHECK(world::is_solid(world::kStateGrass));
    CYANE_CHECK(world::is_solid(static_cast<std::uint16_t>(54 << 4)));  // 箱子
    CYANE_CHECK(!world::is_solid(world::kStateAir));
    for (const std::int32_t raw : {8, 9, 10, 11, 31, 37, 38, 39, 40, 50, 55, 69, 70, 77, 143, 171}) {
        const auto id = static_cast<std::uint16_t>(raw);
        CYANE_CHECK(!world::is_solid(static_cast<std::uint16_t>(id << 4)));
    }
}

// 物理：重力下落并停在地表（脚底 y=4）
CYANE_TEST(mob_falls_and_lands_on_ground) {
    world::World world;
    net::MobManager mobs;
    const auto id = mobs.spawn(90, 0.5, 10.0, 0.5, 0.0f);  // 猪
    CYANE_CHECK(id != 0);
    const std::vector<net::PlayerSnapshot> no_players;
    for (int i = 0; i < 60; ++i) {
        (void)mobs.tick(world, no_players);
    }
    const auto pig = mobs.by_id(id);
    CYANE_CHECK(pig.has_value());
    CYANE_CHECK_NEAR(pig->pos.y, static_cast<double>(kGroundY), 0.001);
    CYANE_CHECK(pig->on_ground);
    CYANE_CHECK_NEAR(pig->velocity_y, 0.0, 0.001);
}

// 物理：撞墙停住；一格高的台阶能上去，两格高上不去
CYANE_TEST(mob_collision_blocks_and_steps) {
    world::World world;
    // 两格高的墙：走不过去
    world.set_block(1, 4, 0, world::kStateStone);
    world.set_block(1, 5, 0, world::kStateStone);
    auto box = world::entity_box(0.5, static_cast<double>(kGroundY), 0.5, 0.9f, 0.9f);
    double dx = 1.0;
    double dy = 0.0;
    double dz = 0.0;
    const auto blocked = world::move_with_collision(world, box, dx, dy, dz, 1.0);
    CYANE_CHECK(blocked.blocked_x);
    CYANE_CHECK(box.max_x <= 1.0 + 1e-6);  // 停在墙面

    // 只有一格台阶：抬腿上台阶（y 上升 1）
    world.set_block(1, 5, 0, world::kStateAir);
    auto step_box = world::entity_box(0.5, static_cast<double>(kGroundY), 0.5, 0.9f, 0.9f);
    dx = 1.0;
    dy = 0.0;
    dz = 0.0;
    const auto stepped = world::move_with_collision(world, step_box, dx, dy, dz, 1.0);
    CYANE_CHECK(stepped.stepped_up);
    CYANE_CHECK_NEAR(step_box.min_y, static_cast<double>(kGroundY) + 1.0, 1e-6);
    CYANE_CHECK(step_box.min_x > 1.0 + 1e-6);
}

// 敌对 AI：僵尸朝玩家逼近，进入近战距离后产生攻击事件
CYANE_TEST(hostile_mob_chases_and_attacks_player) {
    world::World world;
    net::MobManager mobs;
    const auto id = mobs.spawn(54, 0.5, static_cast<double>(kGroundY), 0.5, 0.0f);
    CYANE_CHECK(id != 0);
    const std::vector<net::PlayerSnapshot> players = {player_at(7, 8.5, kGroundY, 0.5)};

    bool attacked = false;
    net::MobAttack attack;
    for (int i = 0; i < 200 && !attacked; ++i) {
        const auto result = mobs.tick(world, players);
        for (const auto& event : result.attacks) {
            if (event.target_player == 7) {
                attacked = true;
                attack = event;
            }
        }
    }
    CYANE_CHECK(attacked);
    CYANE_CHECK_EQ(attack.mob_id, id);
    CYANE_CHECK_EQ(attack.damage, 3.0f);  // 僵尸近战 3.0
    const auto zombie = mobs.by_id(id);
    CYANE_CHECK(zombie.has_value());
    CYANE_CHECK(zombie->pos.x > 0.5);  // 确实朝玩家（+x）移动了
    CYANE_CHECK(zombie->pos.x < 8.5);
}

// 被动 AI：受击后逃窜（朝远离攻击者的方向移动）
CYANE_TEST(passive_mob_retreats_when_hurt) {
    world::World world;
    net::MobManager mobs;
    const auto id = mobs.spawn(90, 0.5, static_cast<double>(kGroundY), 0.5, 0.0f);
    const auto hurt = mobs.damage(id, 1.0f, /*from_x=*/1.5, /*from_z=*/0.5);
    CYANE_CHECK(hurt.found && !hurt.died);
    CYANE_CHECK_NEAR(hurt.health, 9.0f, 0.001);  // 猪血量 10
    const std::vector<net::PlayerSnapshot> no_players;
    for (int i = 0; i < 10; ++i) {
        (void)mobs.tick(world, no_players);
    }
    const auto pig = mobs.by_id(id);
    CYANE_CHECK(pig.has_value());
    // EntityAIPanic：逃向随机落点（未必沿受击反方向），但必定已出发
    CYANE_CHECK(pig->ai == net::MobAi::retreat);
    const double moved = (pig->pos.x - 0.5) * (pig->pos.x - 0.5) +
                         (pig->pos.z - 0.5) * (pig->pos.z - 0.5);
    CYANE_CHECK(moved > 0.01);
}

// 寻路不落崖：vanilla WalkNodeProcessor.getSafePoint 的 maxFallHeight=3 ——
// 单步落差 >3 不可达，只能绕行；绕不过去时返回最接近目标的局部路径（停在崖边）
CYANE_TEST(pathfinder_never_steps_off_a_cliff) {
    world::World world;
    // (5, 0..3, 0) 挖穿：1 格宽、>3 格深
    for (std::int32_t y = 0; y <= 3; ++y) {
        world.set_block(5, y, 0, world::kStateAir);
    }
    const auto path = world::find_path(world, 0.5, 4.0, 0.5, true, 0.9f, 0.9f, 8.5, 4.0, 0.5,
                                       16.0f);
    CYANE_CHECK(path.has_value());
    if (path) {
        CYANE_CHECK(path->points.size() > 1);
        for (std::size_t i = 1; i < path->points.size(); ++i) {
            const auto drop = path->points[i - 1].y - path->points[i].y;
            CYANE_CHECK(drop <= 3);  // maxFallHeight
        }
        for (const auto& node : path->points) {
            CYANE_CHECK(!(node.x == 5 && node.z == 0 && node.y < 4));  // 不进入深坑
        }
    }

    // 深渊宽到绕不过去：只返回走到崖边的局部路径，节点全部留在坑这一侧
    world::World wide;
    for (std::int32_t x = 5; x <= 12; ++x) {
        for (std::int32_t z = -8; z <= 8; ++z) {
            for (std::int32_t y = 0; y <= 3; ++y) {
                wide.set_block(x, y, z, world::kStateAir);
            }
        }
    }
    const auto partial = world::find_path(wide, 0.5, 4.0, 0.5, true, 0.9f, 0.9f, 8.5, 4.0, 0.5,
                                          16.0f);
    CYANE_CHECK(partial.has_value());
    if (partial) {
        for (const auto& node : partial->points) {
            CYANE_CHECK(node.x <= 4);
            CYANE_CHECK(node.y == 4);
        }
    }
}

// 崖边行为：整片挖穿到虚空（绕行半径之外也无路），panic 逃窜 + 漫游期间不会掉下去。
// 原版保证来自寻路（落差 >3 不可达）+ 物理（下方无地面就自由落体）——若实现允许掉下去，
// y 必然跌破地面高度。
CYANE_TEST(mob_does_not_walk_off_ledge) {
    world::World world;
    // x=5 往 +x 挖穿到世界边缘：任何绕行都走不出去
    for (std::int32_t x = 5; x <= 40; ++x) {
        for (std::int32_t z = -40; z <= 40; ++z) {
            for (std::int32_t y = 0; y <= 3; ++y) {
                world.set_block(x, y, z, world::kStateAir);
            }
        }
    }
    net::MobManager mobs;
    const auto id = mobs.spawn(90, 0.5, static_cast<double>(kGroundY), 0.5, 0.0f);
    (void)mobs.damage(id, 1.0f, 0.0, 0.5);  // 触发 panic 逃窜
    double min_y = static_cast<double>(kGroundY);
    for (int i = 0; i < 200; ++i) {
        (void)mobs.tick(world, {});
        const auto pig = mobs.by_id(id);
        CYANE_CHECK(pig.has_value());
        if (!pig) {
            return;
        }
        min_y = std::min(min_y, pig->pos.y);
        CYANE_CHECK(pig->pos.y >= static_cast<double>(kGroundY));  // 始终站在地面上
    }
    const auto pig = mobs.by_id(id);
    CYANE_CHECK_NEAR(min_y, static_cast<double>(kGroundY), 0.001);
    if (pig) {
        // 原版允许走到崖沿（前脚越过边界、身体大部分仍在地面上），
        // 但绝不允许整体越过 —— 走过去必然坠落，y 会掉到地面以下
        CYANE_CHECK(pig->pos.x < 5.45);  // 5.45 = 崖沿 x=5 + 猪半宽 0.45
    }
}

// 骷髅走位崖边保护（EntityMoveHelper STRAFE 迈步探查）：
// 近身后退走位朝向深渊时，vanilla 改为沿朝向（朝玩家）前进，不会走下悬崖
CYANE_TEST(skeleton_strafe_does_not_walk_off_ledge) {
    world::World world;
    // 玩家在 +x 侧（3 格内触发后退走位），-x 侧整片挖空
    for (std::int32_t x = -40; x <= -1; ++x) {
        for (std::int32_t z = -40; z <= 40; ++z) {
            for (std::int32_t y = 0; y <= 3; ++y) {
                world.set_block(x, y, z, world::kStateAir);
            }
        }
    }
    net::MobManager mobs;
    const auto id = mobs.spawn(51, 0.5, static_cast<double>(kGroundY), 0.5, 0.0f);
    std::vector<net::PlayerSnapshot> players;
    players.push_back(player_at(1, 3.5, static_cast<double>(kGroundY), 0.5));
    for (int i = 0; i < 200; ++i) {
        (void)mobs.tick(world, players);
        const auto sk = mobs.by_id(id);
        CYANE_CHECK(sk.has_value());
        if (!sk) {
            return;
        }
        CYANE_CHECK(sk->pos.y >= static_cast<double>(kGroundY));  // 从不坠落
    }
}

// EntityItem 物理：重力 0.04 + 碰撞，落到地面并停住（vanilla tick 顺序）
CYANE_TEST(drop_item_falls_and_lands) {
    world::World world;
    net::ItemDropManager drops;
    const auto id = drops.spawn(0.5, 8.0, 0.5, item::ItemStack{4, 1, 0}, 0.0, 0.0, 0.0, 10);
    CYANE_CHECK(id != 0);
    for (int i = 0; i < 120; ++i) {
        (void)drops.tick(world);
    }
    const auto all = drops.snapshot();
    CYANE_CHECK_EQ(all.size(), std::size_t{1});
    if (!all.empty()) {
        CYANE_CHECK_NEAR(all[0].y, 4.0, 0.01);   // 地面（baseline 草方块顶 = y4）
        CYANE_CHECK(all[0].on_ground);
        CYANE_CHECK_NEAR(all[0].velocity_y, 0.0, 1e-6);
    }
}

// EntityItem 寿命：age 达 6000 消失（vanilla 5 分钟）；restore 带入的 age 继续累计
CYANE_TEST(drop_item_despawns_after_6000_ticks) {
    world::World world;
    net::ItemDropManager drops;
    drops.restore(std::vector<net::DroppedItemState>{
        net::DroppedItemState{0.5, 4.0, 0.5, item::ItemStack{4, 1, 0}, 5999, 0}});
    const auto events = drops.tick(world);
    CYANE_CHECK_EQ(events.destroyed.size(), std::size_t{1});
    CYANE_CHECK_EQ(drops.snapshot().size(), std::size_t{0});
}

// EntityItem 合并：相邻同类堆叠，大者吸收小者，继承 max(delay)/min(age)
CYANE_TEST(drop_items_merge_into_bigger_stack) {
    world::World world;
    net::ItemDropManager drops;
    drops.spawn(0.5, 4.0, 0.5, item::ItemStack{4, 3, 0}, 0.0, 0.0, 0.0, 0);
    drops.spawn(0.8, 4.0, 0.5, item::ItemStack{4, 5, 0}, 0.0, 0.0, 0.0, 20);
    // age % 25 == 0 才触发合并扫描：直接 tick 两下（第 1 tick age=0 触发）
    const auto events = drops.tick(world);
    CYANE_CHECK_EQ(events.merged.size(), std::size_t{1});
    if (!events.merged.empty()) {
        CYANE_CHECK_EQ(events.merged[0].stack.count, std::uint8_t{8});
        CYANE_CHECK_EQ(static_cast<std::uint32_t>(events.merged[0].stack.id), std::uint32_t{4});
    }
    const auto all = drops.snapshot();
    CYANE_CHECK_EQ(all.size(), std::size_t{1});
    if (!all.empty()) {
        CYANE_CHECK_EQ(all[0].stack.count, std::uint8_t{8});
        CYANE_CHECK_EQ(all[0].pickup_delay, 19);  // 继承较大延迟（vanilla 先递减后合并）
    }
    CYANE_CHECK_EQ(events.destroyed.size(), std::size_t{0});  // 合并销毁走 merged 事件
}

// 拾取门控：pickupDelay 未到不进扫描盒；到点后盒相交才拾取（vanilla onCollideWithPlayer）
CYANE_TEST(drop_pickup_gated_by_delay_and_overlap) {
    world::World world;
    net::ItemDropManager drops;
    drops.spawn(0.5, 4.0, 0.5, item::ItemStack{4, 1, 0}, 0.0, 0.0, 0.0, 10);
    const world::Aabb scan{-1.0, 3.0, -1.0, 2.0, 6.0, 2.0};
    CYANE_CHECK_EQ(drops.collect_overlapping(1, scan).size(), std::size_t{0});  // 延迟未到
    for (int i = 0; i < 10; ++i) {
        (void)drops.tick(world);
    }
    const auto picked = drops.collect_overlapping(1, scan);
    CYANE_CHECK_EQ(picked.size(), std::size_t{1});
    if (!picked.empty()) {
        CYANE_CHECK_EQ(picked[0].stack.count, std::uint8_t{1});
    }
    // 远处扫描盒拾不到
    const world::Aabb far{50.0, 3.0, 50.0, 52.0, 6.0, 52.0};
    CYANE_CHECK_EQ(drops.collect_overlapping(1, far).size(), std::size_t{0});
}

// 岩浆烧毁：health 5，岩浆 4 伤害/tick（setOnFireFromLava），两 tick 内消失并报燃烧音
CYANE_TEST(drop_item_burns_in_lava) {
    world::World world;
    world.set_block(0, 4, 0, static_cast<std::uint16_t>(10 << 4));  // 岩浆
    net::ItemDropManager drops;
    drops.spawn(0.5, 4.1, 0.5, item::ItemStack{264, 1, 0}, 0.0, 0.0, 0.0, 0);
    bool burned = false;
    for (int i = 0; i < 10 && !burned; ++i) {
        const auto events = drops.tick(world);
        for (const auto& gone : events.destroyed) {
            if (gone.burn_sound) {
                burned = true;
            }
        }
    }
    CYANE_CHECK(burned);
    CYANE_CHECK_EQ(drops.snapshot().size(), std::size_t{0});
}

// 击退（knockBack 0.4）：水平动量减半后反向叠加，落地时竖直踢 0.4
CYANE_TEST(knockback_pushes_mob_away_and_up) {
    world::World world;
    net::MobManager mobs;
    const auto id = mobs.spawn(90, 0.5, static_cast<double>(kGroundY), 0.5, 0.0f);
    std::vector<net::PlayerSnapshot> settle;
    (void)mobs.tick(world, settle);  // 先落地一拍（onGround 由物理 tick 判定）
    const auto hurt = mobs.damage(id, 1.0f, 5.0, 0.5);  // 攻击者在 +x
    CYANE_CHECK(hurt.found);
    const auto pig = mobs.by_id(id);
    CYANE_CHECK(pig.has_value());
    if (pig) {
        CYANE_CHECK(pig->velocity_x < -0.2);                        // 被推离攻击者
        CYANE_CHECK_NEAR(pig->velocity_y, 0.4, 0.001);              // 落地竖直踢
    }
    std::vector<net::PlayerSnapshot> no_players;
    (void)mobs.tick(world, no_players);
    const auto after = mobs.by_id(id);
    CYANE_CHECK(after.has_value());
    if (after) {
        CYANE_CHECK(after->pos.x < 0.5);  // 本拍实际向 -x 位移
    }
}

// travel 空中分流：离地的生物 AI 控制力几乎为零（jumpMovementFactor 0.02），
// 被击飞期间不会继续走位路线，只沿击退动量飞行
CYANE_TEST(airborne_mob_loses_ai_control) {
    world::World world;
    net::MobManager mobs;
    const auto id = mobs.spawn(54, 0.5, 8.0, 0.5, 0.0f);  // 僵尸悬空
    std::vector<net::PlayerSnapshot> players;
    players.push_back(player_at(1, 0.5, 4.0, 10.0));  // 远处目标 → 追击
    (void)mobs.tick(world, players);
    const auto mob = mobs.by_id(id);
    CYANE_CHECK(mob.has_value());
    if (mob) {
        CYANE_CHECK(!mob->on_ground);
        // 空中加速度 = aim × 0.02 ≈ 0.0046（地面是 aim² ≈ 0.053，旧实现照搬地面值）
        CYANE_CHECK(std::abs(mob->velocity_x) < 0.02);
        CYANE_CHECK(std::abs(mob->velocity_z) < 0.02);
    }
    double drift = 0.0;
    for (int i = 0; i < 5; ++i) {
        (void)mobs.tick(world, players);
        const auto m = mobs.by_id(id);
        if (!m) {
            return;
        }
        drift = std::max(drift, std::abs(m->pos.z - 0.5));
    }
    CYANE_CHECK(drift < 0.05);  // 仍在空中：几乎不朝目标移动
}

// 受伤与死亡：血量递减；死亡进 20 tick 倒地动画期，动画结束才真正移除
CYANE_TEST(mob_damage_and_death) {
    world::World world;
    net::MobManager mobs;
    const auto id = mobs.spawn(93, 0.5, static_cast<double>(kGroundY), 0.5, 0.0f);  // 鸡 4 血
    CYANE_CHECK(id != 0);
    const auto first = mobs.damage(id, 1.0f, 1.5, 0.5);
    CYANE_CHECK(first.found && !first.died);
    CYANE_CHECK_NEAR(first.health, 3.0f, 0.001);
    CYANE_CHECK_EQ(mobs.size(), std::size_t{1});

    const auto lethal = mobs.damage(id, 5.0f, 1.5, 0.5);
    CYANE_CHECK(lethal.found && lethal.died);
    // onDeath：实体保留 20 tick 播倒地动画
    CYANE_CHECK_EQ(mobs.size(), std::size_t{1});
    const std::vector<net::PlayerSnapshot> no_players;
    const auto events = mobs.tick(world, no_players);
    // 首个死亡 tick 发 despawn=false 事件（EntityStatus 3 + 死亡音 + 掉落）
    CYANE_CHECK_EQ(events.deaths.size(), std::size_t{1});
    CYANE_CHECK(!events.deaths[0].despawn);
    for (int i = 0; i < 19; ++i) {
        (void)mobs.tick(world, no_players);
    }
    CYANE_CHECK_EQ(mobs.size(), std::size_t{1});  // 动画未结束
    (void)mobs.tick(world, no_players);
    CYANE_CHECK_EQ(mobs.size(), std::size_t{0});  // 20 tick 到：销毁
    CYANE_CHECK(!mobs.damage(id, 1.0f, 0.0, 0.0).found);  // 已移除：未命中
}

// 生成的生物用全局实体 id 分配器（与玩家/掉落物同空间，避免撞号）
CYANE_TEST(mob_entity_ids_share_global_allocator) {
    world::World world;
    net::MobManager mobs;
    const auto a = mobs.spawn(90, 0.5, static_cast<double>(kGroundY), 0.5, 0.0f);
    const auto b = mobs.spawn(91, 1.5, static_cast<double>(kGroundY), 0.5, 0.0f);
    CYANE_CHECK(a != 0 && b != 0 && a != b);
    // 同一全局分配器：玩家/掉落物/生物共用一段 id（不再有 1000+ 的独立空间）
    CYANE_CHECK_EQ(mobs.spawn(999, 0.5, 4.0, 0.5, 0.0f), std::uint32_t{0});  // 未知物种
}

// SpawnMob (0x03) 载荷：黄金向量（varint id | uuid | varint type | f64 x/y/z | 角度字节 …）
CYANE_TEST(packet_spawn_mob_encodes_vanilla_layout) {
    ByteWriter out;
    net::writers::write_spawn_mob(out, 1, 90, 0.5, 4.0, -0.5, 90.0f, {});
    const auto& data = out.data();
    // id varint(1)=01，uuid 16 字节（只有末两位由 id 合成），type varint(90)=5a
    CYANE_CHECK_EQ(data.size(), std::size_t{1 + 16 + 1 + 24 + 3 + 6 + 1});
    CYANE_CHECK_EQ(std::to_integer<int>(data[0]), 0x01);
    CYANE_CHECK_EQ(std::to_integer<int>(data[17]), 0x5A);  // type 紧接 uuid 之后
    CYANE_CHECK_EQ(std::to_integer<int>(data[16]), 0x01);  // uuid[15] = id & 0xFF
    CYANE_CHECK_EQ(std::to_integer<int>(data[15]), 0x00);  // uuid[14] = id >> 8
    // yaw 90° → 64（1/256 圈）
    const auto yaw_byte = std::to_integer<int>(data[18 + 24]);
    CYANE_CHECK_EQ(yaw_byte, 64);
    CYANE_CHECK_EQ(std::to_integer<int>(data.back()), 0xFF);  // 空 metadata 终止符
}

// 苦力怕：追近后点燃引信（swell state=1，停住），30 tick 后自爆（explosion + death 事件）
CYANE_TEST(creeper_fuse_then_explodes) {
    world::World world;
    net::MobManager mobs;
    const auto id = mobs.spawn(50, 0.5, static_cast<double>(kGroundY), 0.5, 0.0f);
    const std::vector<net::PlayerSnapshot> players = {player_at(7, 2.0, kGroundY, 0.5)};
    bool fused = false;
    for (int i = 0; i < 200 && !fused; ++i) {
        (void)mobs.tick(world, players);
        const auto creeper = mobs.by_id(id);
        fused = creeper && creeper->fuse_state == 1;
    }
    CYANE_CHECK(fused);
    // 引信期间苦力怕不再移动（站定）
    const auto before = mobs.by_id(id);
    CYANE_CHECK(before.has_value());
    const auto x_before = before->pos.x;
    (void)mobs.tick(world, players);
    const auto during = mobs.by_id(id);
    CYANE_CHECK(during.has_value());
    CYANE_CHECK_NEAR(during->pos.x, x_before, 0.001);

    // 走完引信（<=30 tick）：爆炸 + 自爆死亡
    bool exploded = false;
    bool died = false;
    for (int i = 0; i < 40 && !exploded; ++i) {
        const auto result = mobs.tick(world, players);
        exploded = !result.explosions.empty();
        died = !result.deaths.empty();
    }
    CYANE_CHECK(exploded);
    CYANE_CHECK(died);
    CYANE_CHECK_EQ(mobs.size(), std::size_t{0});
    CYANE_CHECK_EQ(mobs.by_id(id).has_value(), false);
}

// 骷髅：射程内按间隔射箭（shots 事件），目标在远处仍保持追击
CYANE_TEST(skeleton_shoots_from_range) {
    world::World world;
    net::MobManager mobs;
    const auto id = mobs.spawn(51, 0.5, static_cast<double>(kGroundY), 0.5, 0.0f);
    const std::vector<net::PlayerSnapshot> players = {player_at(7, 10.5, kGroundY, 0.5)};
    int shots = 0;
    double x_min = 99.0;
    for (int i = 0; i < 120; ++i) {
        const auto result = mobs.tick(world, players);
        shots += static_cast<int>(result.shots.size());
        const auto skeleton = mobs.by_id(id);
        if (skeleton) {
            x_min = std::min(x_min, skeleton->pos.x);
        }
    }
    CYANE_CHECK(shots >= 2);                       // 6 秒内至少射 2 箭
    CYANE_CHECK(x_min < 6.0);                      // 玩家在射程内：骷髅原地射击，不近身
    const auto last = mobs.tick(world, players);
    for (const auto& shot : last.shots) {
        CYANE_CHECK_EQ(shot.target_player, std::uint32_t{7});
    }
    CYANE_CHECK(!last.shots.empty() || shots >= 2);
}

// 箭：重力下坠、命中固体方块产生 ArrowHit、命中后箭消失
CYANE_TEST(projectile_flies_hits_block_and_dies) {
    world::World world;
    net::PlayerHub hub;
    net::MobManager mobs;
    net::ProjectileManager arrows;
    arrows.spawn(1, 0.5, 4.5, 0.5, 0.5, 0.0, 0.0, 2.0f);  // 朝 +x 平射
    bool hit = false;
    for (int i = 0; i < 40 && !hit; ++i) {
        const auto hits = arrows.tick(world, hub, mobs);
        for (const auto& event : hits) {
            CYANE_CHECK(!event.hit_player && !event.hit_mob);
            hit = true;
        }
    }
    CYANE_CHECK(hit);  // 40 tick 内必命中（0.5 格/tick 平射）
    CYANE_CHECK(hit);
    CYANE_CHECK_EQ(arrows.size(), std::size_t{0});
}

// 苦力怕点燃事件：进入 3 格触发一次（不重复），引信中不再触发
CYANE_TEST(creeper_ignition_event_fires_once) {
    world::World world;
    net::MobManager mobs;
    (void)mobs.spawn(50, 0.5, static_cast<double>(kGroundY), 0.5, 0.0f);
    const std::vector<net::PlayerSnapshot> players = {player_at(7, 2.0, kGroundY, 0.5)};
    int ignitions = 0;
    for (int i = 0; i < 60; ++i) {
        const auto result = mobs.tick(world, players);
        ignitions += static_cast<int>(result.ignitions.size());
    }
    CYANE_CHECK_EQ(ignitions, 1);  // 只在点燃瞬间发一次
}

// 逃窜：EntityAIPanic 倍率按物种（猪 1.25/牛 2.0/鸡 1.4），持续 100 tick
CYANE_TEST(retreat_speed_is_below_sprint) {
    world::World world;
    net::MobManager mobs;
    const auto id = mobs.spawn(90, 0.5, static_cast<double>(kGroundY), 0.5, 0.0f);
    (void)mobs.damage(id, 1.0f, 1.5, 0.5);
    const auto pig = mobs.by_id(id);
    CYANE_CHECK(pig.has_value());
    CYANE_CHECK(pig->ai == net::MobAi::retreat);
    CYANE_CHECK_EQ(pig->state_ticks, 100);  // hurtTimestamp 后 100 tick
    // 猪 Panic 1.25：aim = 0.3125，加速度 = aim²，稳态 ≈ 0.215 格/t（≈ 步行速度）
    CYANE_CHECK_NEAR(static_cast<double>(world::mob_type(90)->panic_scale) *
                         static_cast<double>(world::mob_type(90)->speed),
                     0.3125, 0.001);
    const auto cow = world::mob_type(92);
    CYANE_CHECK_NEAR(static_cast<double>(cow->panic_scale), 2.0, 0.001);
}

// 苦力怕引信可熄灭（PathfinderGoalSwell.e()：目标超出 7 格 → state=-1，引信逐 tick 回退）
CYANE_TEST(creeper_fuse_defuses_when_target_flees) {
    world::World world;
    net::MobManager mobs;
    const auto id = mobs.spawn(50, 0.5, static_cast<double>(kGroundY), 0.5, 0.0f);
    std::vector<net::PlayerSnapshot> players = {player_at(7, 2.0, kGroundY, 0.5)};
    for (int i = 0; i < 40; ++i) {
        (void)mobs.tick(world, players);  // 玩家贴脸：引信涨到 30 之前就撤
        const auto creeper = mobs.by_id(id);
        if (creeper && creeper->fuse_state == 1 && creeper->fuse_ticks >= 3) {
            break;
        }
    }
    const auto swelling = mobs.by_id(id);
    CYANE_CHECK(swelling && swelling->fuse_state == 1 && swelling->fuse_ticks > 0);

    // 玩家跑出 7 格（40 格外，引信期苦力怕追不上）：state 变 -1，引信回退到 0，不爆炸
    players = {player_at(7, 40.0, kGroundY, 0.5)};
    bool defuse_event = false;
    bool exploded = false;
    for (int i = 0; i < 60; ++i) {
        const auto result = mobs.tick(world, players);
        defuse_event = defuse_event ||
                       std::any_of(result.ignitions.begin(), result.ignitions.end(),
                                   [](const auto& ign) { return ign.fuse_state < 0; });
        exploded = exploded || !result.explosions.empty();
    }
    CYANE_CHECK(defuse_event);
    CYANE_CHECK(!exploded);
    const auto calmed = mobs.by_id(id);
    CYANE_CHECK(calmed.has_value());
    CYANE_CHECK_EQ(calmed->fuse_ticks, 0);
}

// 爆炸抗性衰减表：vanilla Explosion 的 (resistance+0.3)*0.3 合成值（Cuberite 转写）
CYANE_TEST(explosion_absorption_matches_reference) {
    CYANE_CHECK_NEAR(static_cast<double>(world::explosion_absorption(world::kStateAir)), 0.09,
                     0.0001);
    CYANE_CHECK_NEAR(static_cast<double>(world::explosion_absorption(world::kStateStone)), 1.89,
                     0.0001);
    CYANE_CHECK_NEAR(static_cast<double>(world::explosion_absorption(world::kStateBedrock)),
                     1080000.09, 0.5);  // f32 精度限制
    CYANE_CHECK_NEAR(static_cast<double>(world::explosion_absorption(world::kStateDirt)), 0.24,
                     0.0001);
    // 水/岩浆衰减 100：30.09
    CYANE_CHECK_NEAR(
        static_cast<double>(world::explosion_absorption(static_cast<std::uint16_t>(9 << 4))),
        30.09, 0.0001);
}

// Explosion (0x1C) 黄金向量：位置 f32×3（字段虽是 double 但按 float 写出）+ 半径 +// 记录数 + byte×3 记录 + 动量。位置写成 f64 曾让客户端把记录数读成垃圾值 → OOM。
CYANE_TEST(packet_explosion_uses_float_layout) {
    ByteWriter out;
    std::array<std::array<std::int8_t, 3>, 2> records{{{1, 0, -2}, {-1, -1, 0}}};
    net::writers::write_explosion(out, 4.5, 4.0, -3.5, 3.0f, records);
    // 12(pos) + 4(radius) + 4(count) + 6(records) + 12(motion) = 38
    CYANE_CHECK_EQ(out.data().size(), std::size_t{38});
    // 逐字节比较（线格式大端）：f32 4.5f = 40 90 00 00
    const auto f32_is = [&](std::size_t off, std::uint8_t a, std::uint8_t b, std::uint8_t c,
                            std::uint8_t d) {
        return std::to_integer<int>(out.data()[off]) == a &&
               std::to_integer<int>(out.data()[off + 1]) == b &&
               std::to_integer<int>(out.data()[off + 2]) == c &&
               std::to_integer<int>(out.data()[off + 3]) == d;
    };
    CYANE_CHECK(f32_is(0, 0x40, 0x90, 0x00, 0x00));   // 4.5f
    CYANE_CHECK(f32_is(4, 0x40, 0x80, 0x00, 0x00));   // 4.0f
    CYANE_CHECK(f32_is(8, 0xC0, 0x60, 0x00, 0x00));   // -3.5f
    CYANE_CHECK(f32_is(12, 0x40, 0x40, 0x00, 0x00));  // 3.0f 半径
    // 记录数 = 2（i32 大端，紧随半径）
    CYANE_CHECK_EQ(std::to_integer<int>(out.data()[16]), 0x00);
    CYANE_CHECK_EQ(std::to_integer<int>(out.data()[19]), 0x02);
    // 记录字节：01 00 fe | ff ff 00
    CYANE_CHECK_EQ(std::to_integer<int>(out.data()[20]), 0x01);
    CYANE_CHECK_EQ(std::to_integer<int>(out.data()[22]), 0xFE);  // -2
    CYANE_CHECK_EQ(std::to_integer<int>(out.data()[23]), 0xFF);  // -1
    // 动量 0：末 12 字节全 0
    for (std::size_t i = 26; i < 38; ++i) {
        CYANE_CHECK_EQ(std::to_integer<int>(out.data()[i]), 0x00);
    }
}

// 箭命中玩家：hit_player 带目标 id
CYANE_TEST(projectile_hits_player) {
    world::World world;
    net::PlayerHub hub;
    net::MobManager mobs;
    net::ProjectileManager arrows;
    // 注册一个玩家快照在箭的弹道上
    net::PlayerSnapshot victim;
    victim.entity_id = 9;
    victim.x = 4.0;
    victim.y = static_cast<double>(kGroundY);
    victim.z = 0.5;
    (void)hub.register_player(victim);
    arrows.spawn(1, 0.5, kGroundY + 0.9, 0.5, 0.7, 0.0, 0.0, 2.0f);
    bool hit = false;
    for (int i = 0; i < 30 && !hit; ++i) {
        for (const auto& event : arrows.tick(world, hub, mobs)) {
            hit = event.hit_player;
            if (hit) {
                CYANE_CHECK_EQ(event.target_player, std::uint32_t{9});
            }
        }
    }
    CYANE_CHECK(hit);
}
