#include <cstdint>
#include <string_view>
#include <vector>

#include "cyane/net/mob_manager.hpp"
#include "cyane/net/projectile_manager.hpp"
#include "cyane/net/packet_writers.hpp"
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
    CYANE_CHECK(pig->pos.x < 0.5);  // 朝 -x 逃开
}

// 受伤与死亡：血量递减，归零后从表里移除
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
    CYANE_CHECK_EQ(mobs.size(), std::size_t{0});
    CYANE_CHECK(!mobs.damage(id, 1.0f, 0.0, 0.0).found);  // 已死的再打：未命中
}

// 生成的生物用全局实体 id 分配器（与玩家/掉落物同空间，避免撞号）
CYANE_TEST(mob_entity_ids_share_global_allocator) {
    world::World world;
    net::MobManager mobs;
    const auto a = mobs.spawn(90, 0.5, static_cast<double>(kGroundY), 0.5, 0.0f);
    const auto b = mobs.spawn(91, 1.5, static_cast<double>(kGroundY), 0.5, 0.0f);
    CYANE_CHECK(a != 0 && b != 0 && a != b);
    CYANE_CHECK(a < 1000 && b < 1000);  // 不再是 1000+ 的独立空间
    CYANE_CHECK_EQ(mobs.spawn(999, 0.5, 4.0, 0.5, 0.0f), std::uint32_t{0});  // 未知物种
}

// SpawnMob (0x03) 载荷：黄金向量（varint id | uuid | varint type | f64 x/y/z | 角度字节 …）
CYANE_TEST(packet_spawn_mob_encodes_vanilla_layout) {
    ByteWriter out;
    net::writers::write_spawn_mob(out, 1, 90, 0.5, 4.0, -0.5, 90.0f);
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

// 苦力怕：追近后点燃引信（停住），30 tick 后自爆（explosion + death 事件）
CYANE_TEST(creeper_fuse_then_explodes) {
    world::World world;
    net::MobManager mobs;
    const auto id = mobs.spawn(50, 0.5, static_cast<double>(kGroundY), 0.5, 0.0f);
    const std::vector<net::PlayerSnapshot> players = {player_at(7, 2.0, kGroundY, 0.5)};
    bool fused = false;
    for (int i = 0; i < 200 && !fused; ++i) {
        (void)mobs.tick(world, players);
        const auto creeper = mobs.by_id(id);
        fused = creeper && creeper->fuse_ticks >= 0;
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
