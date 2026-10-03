#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

namespace cyane::world {

// 1.12.2 生物物种表。type 即 SpawnMob(0x03) 的网络实体类型 id，也是刷怪蛋的 damage。
//
// 来源（R-022）：
//   类型 id   — spigot `EntityTypes` 静态注册（0 起自增，与项目既有的猪90/羊91/牛92/鸡93 吻合）
//   尺寸      — 各 Entity* 构造器的 setSize()
//   血量/速度 — 各 Entity* initAttributes() 的 maxHealth / MOVEMENT_SPEED（默认 20）
//   攻击力    — EntityZombie 3.0、EntityMonster 默认 2.0（蜘蛛/骷髅）
//   掉落      — vanilla server.jar 的 assets/minecraft/loot_tables/entities/<name>.json
//   音效 id   — tests/fixtures/sound_registry_ids.txt（同 R-020 的注册表顺序）
struct MobDrop {
    std::int16_t item_id{0};  // 0 = 表尾
    std::int16_t damage{0};
    std::uint8_t min_count{1};
    std::uint8_t max_count{1};
    std::uint8_t chance_percent{100};
};

struct MobType {
    std::int32_t type{0};
    std::string_view name;
    std::string_view nbt_id;
    float width{0.6f};
    float height{1.8f};
    float health{20.0f};
    float speed{0.25f};  // 格/tick
    float attack_damage{2.0f};
    float follow_range{16.0f};  // 目标选择距离（GenericAttributes.FOLLOW_RANGE 默认 16）
    float attack_range{1.2f};   // 进入攻击/开火的距离（骷髅为射程，苦力怕为引信触发距离）
    bool hostile{false};
    bool ranged{false};   // 骷髅：保持距离射箭
    bool explodes{false}; // 苦力怕：近身引信后爆炸
    std::int32_t hurt_sound{0};
    std::int32_t death_sound{0};
    std::array<MobDrop, 4> drops{};
};

[[nodiscard]] constexpr std::optional<MobType> mob_type(std::int32_t type) noexcept {
    switch (type) {
        case 50:  // 苦力怕
            return MobType{.type = 50, .name = "creeper", .nbt_id = "minecraft:creeper",
                           .width = 0.6f, .height = 1.7f, .health = 20.0f, .speed = 0.25f,
                           .attack_range = 3.0f, .hostile = true, .explodes = true, .hurt_sound = 172,
                           .death_sound = 171,
                           .drops = {{{289, 0, 0, 2}, {}, {}, {}}}};
        case 51:  // 骷髅
            return MobType{.type = 51, .name = "skeleton", .nbt_id = "minecraft:skeleton",
                           .width = 0.6f, .height = 1.99f, .health = 20.0f, .speed = 0.25f,
                           .attack_damage = 2.0f, .follow_range = 16.0f, .attack_range = 15.0f,
                           .hostile = true, .ranged = true,
                           .hurt_sound = 406, .death_sound = 405,
                           .drops = {{{262, 0, 0, 2}, {352, 0, 0, 2}, {}, {}}}};
        case 52:  // 蜘蛛
            return MobType{.type = 52, .name = "spider", .nbt_id = "minecraft:spider",
                           .width = 1.4f, .height = 0.9f, .health = 16.0f, .speed = 0.3f,
                           .attack_damage = 2.0f, .attack_range = 1.2f, .hostile = true, .hurt_sound = 431,
                           .death_sound = 430,
                           .drops = {{{287, 0, 0, 2}, {375, 0, 0, 1, 33}, {}, {}}}};
        case 54:  // 僵尸
            return MobType{.type = 54, .name = "zombie", .nbt_id = "minecraft:zombie",
                           .width = 0.6f, .height = 1.95f, .health = 20.0f, .speed = 0.23f,
                           .attack_damage = 3.0f, .follow_range = 35.0f, .attack_range = 1.2f,
                           .hostile = true, .hurt_sound = 485,
                           .death_sound = 484,
                           .drops = {{{367, 0, 0, 2}, {265, 0, 1, 1, 3}, {391, 0, 1, 1, 3},
                                      {392, 0, 1, 1, 3}}}};
        case 90:  // 猪
            return MobType{.type = 90, .name = "pig", .nbt_id = "minecraft:pig", .width = 0.9f,
                           .height = 0.9f, .health = 10.0f, .speed = 0.25f, .hurt_sound = 354,
                           .death_sound = 353, .drops = {{{319, 0, 1, 3}, {}, {}, {}}}};
        case 91:  // 羊
            return MobType{.type = 91, .name = "sheep", .nbt_id = "minecraft:sheep", .width = 0.9f,
                           .height = 1.3f, .health = 8.0f, .speed = 0.23f, .hurt_sound = 387,
                           .death_sound = 386,
                           .drops = {{{423, 0, 1, 2}, {35, 0, 1, 1}, {}, {}}}};
        case 92:  // 牛
            return MobType{.type = 92, .name = "cow", .nbt_id = "minecraft:cow", .width = 0.9f,
                           .height = 1.4f, .health = 10.0f, .speed = 0.2f, .hurt_sound = 168,
                           .death_sound = 167,
                           .drops = {{{334, 0, 0, 2}, {363, 0, 1, 3}, {}, {}}}};
        case 93:  // 鸡
            return MobType{.type = 93, .name = "chicken", .nbt_id = "minecraft:chicken", .width = 0.4f,
                           .height = 0.7f, .health = 4.0f, .speed = 0.25f, .hurt_sound = 164,
                           .death_sound = 162, .drops = {{{288, 0, 0, 2}, {365, 0, 1, 1}, {}, {}}}};
        default:
            return std::nullopt;
    }
}

// 存档 NBT 的 id → 网络类型 id（非本表物种返回 nullopt，调用方原样透传）
[[nodiscard]] constexpr std::optional<std::int32_t> mob_type_from_nbt(
    std::string_view nbt_id) noexcept {
    if (nbt_id == "minecraft:creeper") {
        return 50;
    }
    if (nbt_id == "minecraft:skeleton") {
        return 51;
    }
    if (nbt_id == "minecraft:spider") {
        return 52;
    }
    if (nbt_id == "minecraft:zombie") {
        return 54;
    }
    if (nbt_id == "minecraft:pig") {
        return 90;
    }
    if (nbt_id == "minecraft:sheep") {
        return 91;
    }
    if (nbt_id == "minecraft:cow") {
        return 92;
    }
    if (nbt_id == "minecraft:chicken") {
        return 93;
    }
    return std::nullopt;
}

// 刷怪蛋（item 383）的 damage 就是实体类型 id
[[nodiscard]] constexpr std::optional<std::int32_t> spawn_egg_type(std::int16_t item_id,
                                                                   std::int16_t damage) noexcept {
    if (item_id != 383) {
        return std::nullopt;
    }
    if (auto type = mob_type(damage); type.has_value()) {
        return type->type;
    }
    return std::nullopt;
}

}  // namespace cyane::world
