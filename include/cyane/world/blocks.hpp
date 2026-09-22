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

inline constexpr std::size_t kSectionBlockCount = 16 * 16 * 16;
inline constexpr std::size_t kLightArrayBytes = kSectionBlockCount / 2;

// y<<8 | z<<4 | x，与 DataPaletteBlock.b(x, y, z) 的线性索引一致
[[nodiscard]] constexpr std::size_t section_index(std::size_t x, std::size_t y, std::size_t z) noexcept {
    return (y << 8) | (z << 4) | x;
}

}
