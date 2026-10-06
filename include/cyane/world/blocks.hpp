#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace cyane::world {

// 1.12.2 全局状态 id = blockId << 4 | meta（Block.REGISTRY_ID.getId）
inline constexpr std::uint16_t kStateAir = 0;
inline constexpr std::uint16_t kStateStone = 1 << 4;
inline constexpr std::uint16_t kStateGrass = 2 << 4;
inline constexpr std::uint16_t kStateDirt = 3 << 4;
inline constexpr std::uint16_t kStateBedrock = 7 << 4;
inline constexpr std::uint16_t kStateDispenser = 23 << 4;      // 发射器
inline constexpr std::uint16_t kStateChest = 54 << 4;          // 箱子方块 id=54
inline constexpr std::uint16_t kStateCraftingTable = 58 << 4;  // 工作台方块 id=58
inline constexpr std::uint16_t kStateFurnace = 61 << 4;        // 熔炉方块 id=61
inline constexpr std::uint16_t kStateHopper = 154 << 4;        // 漏斗
inline constexpr std::uint16_t kStateDropper = 158 << 4;       // 投掷器

[[nodiscard]] constexpr std::uint16_t block_id(std::uint16_t state) noexcept { return state >> 4; }
[[nodiscard]] constexpr std::uint16_t state_meta(std::uint16_t state) noexcept { return state & 0x0F; }

// 1.12.2 中方块型物品的 item id 与 block id 同值（id < 256），
// 物品 damage 即方块 meta，故状态 = itemId<<4 | (damage & 0xF)。
// 门是例外：纯物品、item id 与 block id 不同值（见 door_block_from_item）。
// 返回 kStateAir 表示该物品不是可放置方块。
[[nodiscard]] constexpr std::uint16_t door_block_from_item(std::int16_t item_id) noexcept {
    switch (item_id) {
        case 324: return 64;   // 橡木门
        case 330: return 71;   // 铁门
        case 427: return 193;  // 云杉门
        case 428: return 194;  // 白桦门
        case 429: return 195;  // 丛林木门
        case 430: return 196;  // 金合欢门
        case 431: return 197;  // 深色橡木门
        default: return 0;
    }
}

[[nodiscard]] constexpr std::uint16_t block_state_from_item(std::int16_t item_id,
                                                            std::int16_t damage) noexcept {
    if (item_id <= 0) {
        return kStateAir;
    }
    if (item_id >= 256) {
        const auto door = door_block_from_item(item_id);
        return door != 0 ? static_cast<std::uint16_t>(door << 4) : kStateAir;
    }
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(item_id) << 4) |
                                      (static_cast<std::uint16_t>(damage) & 0x0F));
}

// PlayerDigging/BlockPlacement 的 face（EnumDirection 序号）对应的坐标增量
struct BlockFaceDelta {
    std::int32_t dx{0};
    std::int32_t dy{0};
    std::int32_t dz{0};
};

[[nodiscard]] constexpr BlockFaceDelta face_delta(std::int32_t face) noexcept {
    switch (face) {
        case 0: return {0, -1, 0};  // down
        case 1: return {0, 1, 0};   // up
        case 2: return {0, 0, -1};  // north
        case 3: return {0, 0, 1};   // south
        case 4: return {-1, 0, 0};  // west
        case 5: return {1, 0, 0};   // east
        default: return {};
    }
}

// 木门：64（橡木）+ 193..197（云杉/白桦/丛林/金合欢/深色橡木），可手动开关
[[nodiscard]] constexpr bool is_wooden_door(std::uint16_t block_id) noexcept {
    return block_id == 64 || (block_id >= 193 && block_id <= 197);
}

// 门类（含铁门 71）：上下双半方块
[[nodiscard]] constexpr bool is_door(std::uint16_t block_id) noexcept {
    return is_wooden_door(block_id) || block_id == 71;
}

[[nodiscard]] constexpr bool is_button(std::uint16_t block_id) noexcept {
    return block_id == 77 || block_id == 143;
}

// 拉杆（69）：附着型方块，支撑没了同样掉落
[[nodiscard]] constexpr bool is_lever(std::uint16_t block_id) noexcept {
    return block_id == 69;
}

// 附着型方块（按钮/拉杆）meta 低 3 位 = FACING（EnumDirection，由支撑方块指向自身；
// BlockButtonAbstract.fromLegacyData：0=下 1=东 2=西 3=南 4=北 5=上）。
// 返回从自身指向支撑方块的偏移——支撑方块没了，该方块就该掉落。
[[nodiscard]] constexpr BlockFaceDelta support_delta(std::uint16_t meta) noexcept {
    switch (meta & 0x07) {
        case 0: return {0, 1, 0};    // 贴天花板：支撑在上方
        case 1: return {-1, 0, 0};   // 朝东：支撑在西侧
        case 2: return {1, 0, 0};    // 朝西：支撑在东侧
        case 3: return {0, 0, -1};   // 朝南：支撑在北侧
        case 4: return {0, 0, 1};    // 朝北：支撑在南侧
        default: return {0, -1, 0};  // 贴地板：支撑在下方
    }
}

// 附着方块 state 的支撑是否位于相对自身 (dx,dy,dz) 处（破坏支撑后据此判定掉落）
[[nodiscard]] constexpr bool supported_by(std::uint16_t state, std::int32_t dx, std::int32_t dy,
                                          std::int32_t dz) noexcept {
    const auto support = support_delta(state_meta(state));
    return support.dx == dx && support.dy == dy && support.dz == dz;
}

// EntityItem 摩擦用：方块滑腻度（vanilla Block.slipperiness；地面摩擦 = 此值 × 0.98）
[[nodiscard]] constexpr double block_slipperiness(std::uint16_t state) noexcept {
    switch (block_id(state)) {
        case 79: case 174: case 212: return 0.98;  // 冰 / 浮冰 / 霜冰
        case 165: return 0.8;                      // 粘液块
        default: return 0.6;
    }
}

// 实体碰撞用：该方块是否阻挡移动。默认固体，逐项排除无碰撞盒的方块
// （气体/液体/花草/线缆/轨道/火把/压力板/按钮拉杆/告示牌/作物/传送门/地毯等）。
[[nodiscard]] constexpr bool is_solid(std::uint16_t state) noexcept {
    switch (block_id(state)) {
        case 0:              // 空气
        case 6:              // 树苗
        case 8: case 9:      // 水
        case 10: case 11:    // 岩浆
        case 27: case 28:    // 铁轨
        case 31:             // 草
        case 32:             // 枯灌木
        case 34: case 36:    // 活塞头 / 移动中的活塞
        case 37: case 38:    // 花
        case 39: case 40:    // 蘑菇
        case 50:             // 火把
        case 51:             // 火
        case 55:             // 红石线
        case 59:             // 小麦
        case 63: case 68:    // 告示牌
        case 65:             // 梯子
        case 69:             // 拉杆
        case 70: case 72: case 147: case 148:  // 压力板
        case 75: case 76:    // 红石火把
        case 77: case 143:   // 按钮
        case 78:             // 雪层
        case 83:             // 甘蔗
        case 90: case 119:   // 传送门
        case 104: case 105:  // 西瓜/南瓜茎
        case 106:            // 藤蔓
        case 111:            // 睡莲
        case 115:            // 地狱疣
        case 127:            // 可可豆
        case 131: case 132:  // 绊线钩 / 绊线
        case 140:            // 花盆
        case 141: case 142:  // 胡萝卜 / 马铃薯
        case 157:            // 激活铁轨
        case 171:            // 地毯
        case 175:            // 双植
        case 176: case 177:  // 旗帜
        case 244:            // 甜菜
            return false;
        default:
            return true;
    }
}

inline constexpr std::size_t kSectionBlockCount = 16 * 16 * 16;
inline constexpr std::size_t kLightArrayBytes = kSectionBlockCount / 2;

// y<<8 | z<<4 | x，与 DataPaletteBlock.b(x, y, z) 的线性索引一致
[[nodiscard]] constexpr std::size_t section_index(std::size_t x, std::size_t y, std::size_t z) noexcept {
    return (y << 8) | (z << 4) | x;
}

// 方块世界坐标打包成 64 位键（26 位 x | 12 位 y | 26 位 z），容器/方块实体统一索引
struct BlockKeyPos {
    std::int32_t x{0};
    std::int32_t y{0};
    std::int32_t z{0};
};

[[nodiscard]] constexpr std::int64_t pack_block_pos(std::int32_t x, std::int32_t y,
                                                    std::int32_t z) noexcept {
    return (static_cast<std::int64_t>(x & 0x3FFFFFF) << 38) |
           (static_cast<std::int64_t>(y & 0xFFF) << 26) |
           static_cast<std::int64_t>(z & 0x3FFFFFF);
}

[[nodiscard]] constexpr BlockKeyPos unpack_block_pos(std::int64_t key) noexcept {
    const auto extend = [](std::uint64_t value, int bits) noexcept {
        const std::uint64_t mask = (1ull << bits) - 1;
        const std::uint64_t sign = 1ull << (bits - 1);
        return static_cast<std::int32_t>(((value & mask) ^ sign) - sign);
    };
    const auto raw = static_cast<std::uint64_t>(key);
    return {extend(raw >> 38, 26), extend(raw >> 26, 12), extend(raw, 26)};
}

// 超平坦 baseline（世界层公共契约；持久化编解码据此剔除冗余状态）
[[nodiscard]] constexpr std::uint16_t flat_baseline(std::int32_t wy) noexcept {
    switch (wy) {
        case 0: return kStateBedrock;
        case 1:
        case 2: return kStateDirt;
        case 3: return kStateGrass;
        default: return kStateAir;
    }
}

// 爆炸射线对一方块的强度衰减（vanilla Explosion: f -= (resistance + 0.3) * 0.3，
// 此处为已含 0.3 系数的合成值）。抗性取自 Cuberite Explodinator::GetExplosionAbsorption
// （wiki 爆炸抗性表），只列出 1.12.2 常见方块，其余按默认 0.09（resistance 0）。
[[nodiscard]] constexpr float explosion_absorption(std::uint16_t state) noexcept {
    switch (block_id(state)) {
        case 7:                       // 基岩 3600000
            return 1080000.09f;
        case 49:                      // 黑曜石 1200
        case 145:                     // 铁砧 1200
        case 138:                     // 信标 1500? 仍归入不可炸类
            return 360.09f;
        case 8: case 9: case 10: case 11:  // 水/岩浆 100
            return 30.09f;
        case 130:                     // 末影箱 600
            return 180.09f;
        case 121:                     // 末地石 9
            return 2.79f;
        case 1:                       // 石头 6
        case 4:                       // 圆石 6
        case 98:                      // 石砖 6
        case 15: case 16: case 21: case 56: case 73: case 74:  // 矿石类 6
        case 41: case 42: case 57: case 133:                   // 金属块 6
        case 45:                      // 砖 6
        case 112:                     // 地狱砖 6
        case 139:                     // 圆石墙 6
            return 1.89f;
        case 71:                      // 铁门 5
        case 52:                      // 刷怪笼 5
            return 1.59f;
        case 154:                     // 漏斗 4.8
            return 1.53f;
        case 61: case 62: case 23: case 158:  // 熔炉/发射器/投掷器 3.5
            return 1.14f;
        case 30:                      // 蛛网 4
            return 1.29f;
        case 5: case 18: case 20: case 25: case 26:
        case 43: case 44: case 53: case 67: case 85: case 96: case 108:
        case 109: case 114: case 128: case 134: case 135: case 136:
        case 156: case 163: case 164:  // 木板/羊毛/楼梯/栅栏/木门类 3
            return 0.99f;
        case 54: case 58: case 146:   // 箱子/工作台 2.5
            return 0.84f;
        case 162:                     // 原木 2
            return 0.69f;
        case 47:                      // 书架 1.5
            return 0.54f;
        case 24: case 179: case 180:  // 砂岩/红砂岩 0.8
        case 35:                      // 羊毛 0.8
            return 0.33f;
        case 2: case 13: case 60: case 82: case 19:  // 草方块/砂砾/耕地/黏土/海绵 0.6
            return 0.27f;
        case 3: case 12: case 79: case 88: case 170: case 236:  // 泥土/沙/冰/灵魂沙/干草块/混凝土粉末 0.5
            return 0.24f;
        case 155: case 123: case 124:  // 石英块/红石灯 0.8
            return 0.33f;
        default:
            return 0.09f;
    }
}

}
