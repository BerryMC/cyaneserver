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
inline constexpr std::uint16_t kStateChest = 54 << 4;    // 箱子方块 id=54
inline constexpr std::uint16_t kStateCraftingTable = 58 << 4; // 工作台方块 id=58
inline constexpr std::uint16_t kStateFurnace = 61 << 4;  // 熔炉方块 id=61

[[nodiscard]] constexpr std::uint16_t block_id(std::uint16_t state) noexcept { return state >> 4; }
[[nodiscard]] constexpr std::uint16_t state_meta(std::uint16_t state) noexcept { return state & 0x0F; }

// 1.12.2 中方块型物品的 item id 与 block id 同值（id < 256），
// 物品 damage 即方块 meta，故状态 = itemId<<4 | (damage & 0xF)。
// 返回 kStateAir 表示该物品不是可放置方块。
[[nodiscard]] constexpr std::uint16_t block_state_from_item(std::int16_t item_id,
                                                            std::int16_t damage) noexcept {
    if (item_id <= 0 || item_id >= 256) {
        return kStateAir;
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

}
