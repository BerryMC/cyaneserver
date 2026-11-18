#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "cyane/world/blocks.hpp"

namespace cyane::item {

// 1.12.2 工具分类与等级（等级决定能采集哪些方块）：
// wood/gold=1、stone=2、iron=3、diamond=4
enum class ToolKind : std::uint8_t { none, pickaxe, axe, shovel, sword, shears };
enum class ToolTier : std::uint8_t { hand = 0, wood = 1, stone = 2, iron = 3, diamond = 4 };

struct ToolInfo {
    ToolKind kind{ToolKind::none};
    ToolTier tier{ToolTier::hand};
};

// 护甲值（Armor points，1.12.2 各护甲件）
[[nodiscard]] inline int armor_points(std::int16_t item_id) {
    switch (item_id) {
        case 298: return 1; case 299: return 3; case 300: return 2; case 301: return 1;  // 皮革
        case 302: return 2; case 303: return 5; case 304: return 4; case 305: return 1;  // 锁链
        case 306: return 2; case 307: return 6; case 308: return 5; case 309: return 2;  // 铁
        case 310: return 3; case 311: return 8; case 312: return 6; case 313: return 3;  // 钻石
        case 314: return 2; case 315: return 5; case 316: return 3; case 317: return 1;  // 金
        default: return 0;
    }
}

// EntityLiving.getSlotForItemStack 的目标槽（ContainerPlayer 布局）：
// 返回 5..8 = 头盔/胸甲/护腿/靴子，45 = 副手，-1 = 无专用槽（走主背包）。
// 护甲 index 为 FEET=0/LEGS=1/CHEST=2/HEAD=3，ContainerPlayer 用 8-index。
[[nodiscard]] constexpr int equipment_slot(std::int16_t item_id) noexcept {
    switch (item_id) {
        case 298: case 302: case 306: case 310: case 314: return 5;  // 头盔
        case 86: case 397: return 5;                                 // 南瓜 / 头颅 → HEAD
        case 299: case 303: case 307: case 311: case 315: return 6;  // 胸甲
        case 443: return 6;                                          // 鞘翅 → CHEST
        case 300: case 304: case 308: case 312: case 316: return 7;  // 护腿
        case 301: case 305: case 309: case 313: case 317: return 8;  // 靴子
        case 442: return 45;                                         // 盾牌 → OFFHAND
        default: return -1;
    }
}

// SharedMonsterAttributes.ATTACK_SPEED：基础 4.0，剑 -2.4、斧按材质 -3.2..-3.0
[[nodiscard]] constexpr float attack_speed(std::int16_t item_id) noexcept {
    switch (item_id) {
        case 268: case 272: case 267: case 276: case 283: return 1.6f;  // 剑（全材质）
        case 271: case 275: return 0.8f;                                // 木斧 / 石斧
        case 258: return 0.9f;                                          // 铁斧
        case 279: case 286: return 1.0f;                                // 钻石斧 / 金斧
        default: return 4.0f;                                           // 空手/其它工具
    }
}

// 物品最大耐久（Item.setMaxDamage）。数值来自原版 ToolMaterial 与
// ItemArmor.ArmorMaterial：工具五档 59/131/250/1561/32，
// 护甲 = MAX_DAMAGE_ARRAY{13,15,16,11} × 材质系数（皮革5/锁链15/铁15/金7/钻石33）。
[[nodiscard]] constexpr int max_damage(std::int16_t item_id) noexcept {
    switch (item_id) {
        // 木
        case 268: case 269: case 270: case 271: case 290: return 59;
        // 石
        case 272: case 273: case 274: case 275: case 291: return 131;
        // 铁
        case 256: case 257: case 258: case 267: case 292: return 250;
        // 钻石
        case 276: case 277: case 278: case 279: case 293: return 1561;
        // 金
        case 283: case 284: case 285: case 286: case 294: return 32;
        // 皮革：头/胸/腿/脚
        case 298: return 55; case 299: return 80; case 300: return 75; case 301: return 65;
        // 锁链
        case 302: return 165; case 303: return 240; case 304: return 225; case 305: return 195;
        // 铁
        case 306: return 165; case 307: return 240; case 308: return 225; case 309: return 195;
        // 钻石
        case 310: return 363; case 311: return 528; case 312: return 495; case 313: return 429;
        // 金
        case 314: return 77; case 315: return 112; case 316: return 105; case 317: return 91;
        case 261: return 384;   // 弓
        case 346: return 64;    // 钓鱼竿
        case 259: return 64;    // 打火石
        case 359: return 238;   // 剪刀
        case 442: return 336;   // 盾牌
        case 443: return 432;   // 鞘翅
        default: return 0;      // 不可损坏
    }
}

// ArmorUtil.getDamageAfterAbsorb：damage×(1 − clamp(armor − damage/(2+tough/4), armor×0.2, 20)/25)
[[nodiscard]] inline float damage_after_armor(float damage, int armor, float toughness) {
    const float f = 2.0f + toughness / 4.0f;
    const float capped = std::clamp(static_cast<float>(armor) - damage / f,
                                    static_cast<float>(armor) * 0.2f, 20.0f);
    return damage * (1.0f - capped / 25.0f);
}

[[nodiscard]] inline ToolInfo tool_of(std::int16_t item_id) {
    switch (item_id) {
        case 270: return {ToolKind::pickaxe, ToolTier::wood};
        case 285: return {ToolKind::pickaxe, ToolTier::wood};   // 金镐=木级
        case 274: return {ToolKind::pickaxe, ToolTier::stone};
        case 257: return {ToolKind::pickaxe, ToolTier::iron};
        case 278: return {ToolKind::pickaxe, ToolTier::diamond};
        case 271: return {ToolKind::axe, ToolTier::wood};
        case 286: return {ToolKind::axe, ToolTier::wood};
        case 275: return {ToolKind::axe, ToolTier::stone};
        case 258: return {ToolKind::axe, ToolTier::iron};
        case 279: return {ToolKind::axe, ToolTier::diamond};
        case 269: return {ToolKind::shovel, ToolTier::wood};
        case 284: return {ToolKind::shovel, ToolTier::wood};
        case 273: return {ToolKind::shovel, ToolTier::stone};
        case 256: return {ToolKind::shovel, ToolTier::iron};
        case 277: return {ToolKind::shovel, ToolTier::diamond};
        case 268: return {ToolKind::sword, ToolTier::wood};
        case 283: return {ToolKind::sword, ToolTier::wood};
        case 272: return {ToolKind::sword, ToolTier::stone};
        case 267: return {ToolKind::sword, ToolTier::iron};
        case 276: return {ToolKind::sword, ToolTier::diamond};
        case 359: return {ToolKind::shears, ToolTier::hand};
        default: return {ToolKind::none, ToolTier::hand};
    }
}

// 挖掘速度倍率：正确工具类别的材质加成（1.12.2 基础值，不含效率附魔）
[[nodiscard]] inline float dig_speed_multiplier(std::int16_t item_id) {
    const auto info = tool_of(item_id);
    switch (info.kind) {
        case ToolKind::pickaxe:
        case ToolKind::axe:
        case ToolKind::shovel:
            switch (info.tier) {
                case ToolTier::wood: return 2.0f;
                case ToolTier::stone: return 4.0f;
                case ToolTier::iron: return 6.0f;
                case ToolTier::diamond: return 8.0f;
                default: return 12.0f;  // gold
            }
        case ToolKind::sword: return 1.5f;
        default: return 1.0f;
    }
}

[[nodiscard]] constexpr bool is_pickaxe_block(std::uint16_t id) noexcept {
    switch (id) {
        case 1: case 4: case 14: case 15: case 16: case 21: case 24:
        case 41: case 42: case 43: case 44: case 45: case 48: case 49:
        case 56: case 57: case 61: case 62: case 70: case 73: case 74:
        case 87: case 98: case 101: case 112: case 116: case 121: case 129:
        case 133: case 138: case 139: case 145: case 153: case 155: case 172:
        case 179: case 213:
            return true;
        default:
            return false;
    }
}

[[nodiscard]] constexpr bool is_axe_block(std::uint16_t id) noexcept {
    switch (id) {
        case 5: case 17: case 47: case 53: case 54: case 58: case 64:
        case 85: case 96: case 107: case 125: case 126: case 134: case 135:
        case 136: case 146: case 162: case 163: case 164: case 183: case 184:
        case 185: case 186: case 187: case 193: case 194: case 195: case 196:
        case 197:
            return true;
        default:
            return false;
    }
}

[[nodiscard]] constexpr bool is_shovel_block(std::uint16_t id) noexcept {
    switch (id) {
        case 2: case 3: case 12: case 13: case 60: case 78: case 80:
        case 82: case 88: case 198: case 208:
            return true;
        default:
            return false;
    }
}

[[nodiscard]] inline float tool_dig_speed(std::uint16_t block_id, std::int16_t item_id) noexcept {
    const auto info = tool_of(item_id);
    const float speed = dig_speed_multiplier(item_id);
    if (is_pickaxe_block(block_id) && info.kind == ToolKind::pickaxe) return speed;
    if (is_axe_block(block_id) && info.kind == ToolKind::axe) return speed;
    if (is_shovel_block(block_id) && info.kind == ToolKind::shovel) return speed;
    if (block_id == 30 && info.kind == ToolKind::sword) return 15.0f;
    if (block_id == 18 && info.kind == ToolKind::shears) return 15.0f;
    return 1.0f;
}

[[nodiscard]] inline float dig_duration_ms(std::uint16_t block_id, std::int16_t held_item, bool can_harvest) noexcept {
    const float hardness = world::block_hardness(block_id);
    if (hardness < 0.0f) return 1e9f;
    if (hardness == 0.0f) return 50.0f;
    const float speed = tool_dig_speed(block_id, held_item);
    const float damage_per_tick = speed / hardness / (can_harvest ? 30.0f : 100.0f);
    if (damage_per_tick >= 1.0f) return 50.0f;
    const float total_ticks = std::ceil(1.0f / damage_per_tick);
    return total_ticks * 50.0f;
}

} // namespace cyane::item
