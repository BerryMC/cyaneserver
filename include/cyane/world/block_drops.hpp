#pragma once

#include <cstdint>
#include <vector>

#include "cyane/world/blocks.hpp"

namespace cyane::world {

// 1.12.2 方块破坏掉落（生存模式）。未列出的方块掉自身（id 同值、meta 作 damage）。
// 需要工具等级才掉落的规则（如无镐挖石不掉）暂不建模，统一掉落。
struct DropEntry {
    std::int16_t item_id;
    std::int16_t damage{0};
    std::uint8_t count{1};
};

[[nodiscard]] inline std::vector<DropEntry> block_drops(std::uint16_t state) {
    const auto id = block_id(state);
    const auto meta = state_meta(state);
    if (id == 0) {
        return {};  // 空气
    }
    switch (id) {
        case 1: return {{4, 0, 1}};              // 石头 → 圆石
        case 2: return {{3, 0, 1}};              // 草方块 → 泥土
        case 16: return {{263, 0, 1}};           // 煤矿石 → 煤炭
        case 21: return {{351, 4, 4}};           // 青金石矿 → 4 青金石
        case 56: return {{264, 0, 1}};           // 钻石矿 → 钻石
        case 73: return {{331, 0, 4}};           // 红石矿 → 4 红石
        case 74: return {{331, 0, 4}};           // 红石矿（点亮态）
        case 129: return {{388, 0, 1}};          // 绿宝石矿
        case 153: return {{406, 0, 1}};          // 石英矿
        case 82: return {{337, 0, 4}};           // 黏土块 → 4 黏土球
        case 30: return {{32, 0, 1}};            // 蛛网 → 线
        case 55: return {{331, 0, 1}};           // 红石线 → 红石
        case 63: return {{323, 0, 1}};           // 告示牌（立）→ 牌
        case 68: return {{323, 0, 1}};           // 告示牌（墙）
        case 26: return {{355, static_cast<std::int16_t>(meta), 1}};  // 床 → 床物品
        case 20: return {};                      // 玻璃 → 无
        case 18: return {};                      // 树叶 → 无（树苗/苹果概率后补）
        case 79: return {};                      // 冰 → 无
        case 59: return {};                      // 小麦作物 → 无（种子掉落后补）
        case 104: return {};                     // 西瓜茎
        case 105: return {};                     // 南瓜茎
        case 102: return {};                     // 玻璃板
        case 119: return {};                     // 末地传送门
        case 90: return {};                      // 下界传送门
        case 7: return {};                       // 基岩
        case 9: case 8: return {};               // 静止/流动水
        case 11: case 10: return {};             // 静止/流动岩浆
        default: return {{static_cast<std::int16_t>(id), static_cast<std::int16_t>(meta), 1}};
    }
}

} // namespace cyane::world
