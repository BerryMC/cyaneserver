#pragma once

#include <algorithm>
#include <cstdint>

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

} // namespace cyane::item
