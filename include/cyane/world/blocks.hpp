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

}
