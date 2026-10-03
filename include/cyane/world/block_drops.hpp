#pragma once

#include <cstdint>
#include <random>
#include <vector>

#include "cyane/item/item_tools.hpp"
#include "cyane/world/blocks.hpp"

namespace cyane::world {

// 1.12.2 方块破坏掉落（生存模式）。未列出的方块掉自身（id 同值、meta 作 damage）。
// 采集资格与 Cuberite ItemPickaxe 同表：需镐而不满足时方块照样被破坏但不掉落。
struct DropEntry {
    std::int16_t item_id;
    std::int16_t damage{0};
    std::uint8_t count{1};
};

struct HarvestRule {
    bool needs_pickaxe{false};
    std::uint8_t min_tier{1};  // item::ToolTier：1=wood/gold 2=stone 3=iron 4=diamond
};

[[nodiscard]] inline HarvestRule harvest_rule(std::uint16_t block_id) {
    switch (block_id) {
        case 49: return {true, 4};  // 黑曜石（钻石镐）
        // 铁级：钻石矿/金矿/绿宝石矿/红石矿/钻石块/金块
        case 56: case 14: case 129: case 73: case 74: case 57: case 41:
            return {true, 3};
        // 石级：铁矿/铁块/青金矿/青金块
        case 15: case 42: case 21: case 22:
            return {true, 2};
        // 木级（任意镐）：石头/圆石/煤矿/砂石/下界岩/砖/熔炉/发射器/投掷器/
        // 铁栏杆/末地石/石砖/红沙石/石英块/苔石/硬化黏土/铁砧/附魔台/炼药锅/
        // 漏斗/刷怪笼/岩浆块/下界砖/石砖楼梯/石压力板/石台阶系列
        case 1: case 4: case 16: case 24: case 87: case 45: case 61: case 62:
        case 23: case 158: case 101: case 121: case 98: case 179: case 155:
        case 48: case 172: case 145: case 116: case 138: case 154: case 52:
        case 213: case 112: case 139: case 70: case 43: case 44:
            return {true, 1};
        default: return {false, 0};
    }
}

// 满足采集资格则返回真实掉落；不满足返回空（方块仍被破坏，vanilla 行为）
[[nodiscard]] inline std::vector<DropEntry> block_drops(std::uint16_t state,
                                                        item::ToolInfo tool) {
    const auto id = block_id(state);
    if (id == 0) {
        return {};
    }
    const auto rule = harvest_rule(id);
    if (rule.needs_pickaxe) {
        const bool ok = tool.kind == item::ToolKind::pickaxe &&
                        static_cast<std::uint8_t>(tool.tier) >= rule.min_tier;
        if (!ok) {
            return {};
        }
    }
    const auto meta = state_meta(state);
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
        case 13: {                                // 砾石 → 10% 燧石，否则砾石
            static std::mt19937 engine{std::random_device{}()};
            if (std::uniform_int_distribution<int>(0, 99)(engine) < 10) {
                return {{318, 0, 1}};
            }
            return {{13, 0, 1}};
        }
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
