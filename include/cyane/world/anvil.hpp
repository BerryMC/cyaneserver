#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

#include "cyane/core/bytes.hpp"
#include "cyane/core/error.hpp"
#include "cyane/item/item_stack.hpp"
#include "cyane/world/chunk.hpp"

namespace cyane::world {

// 箱子内容（与 net::ContainerStore::Chest 布局一致，避免 world→net 反向依赖）
inline constexpr std::size_t kChestSlots = 27;
using StoredChest = std::array<item::ItemStack, kChestSlots>;

// 熔炉持久化状态（槽位 + 燃烧/冶炼进度）
struct StoredFurnace {
    item::ItemStack input;
    item::ItemStack fuel;
    item::ItemStack output;
    std::int32_t burn_left{0};
    std::int32_t burn_total{0};
    std::int32_t cook_time{0};
};

// 一个区块的方块实体（按方块位置键 pack_block_pos 索引）
struct ChunkEntities {
    std::vector<std::pair<std::int64_t, StoredChest>> chests;
    std::vector<std::pair<std::int64_t, StoredFurnace>> furnaces;
    [[nodiscard]] bool empty() const noexcept { return chests.empty() && furnaces.empty(); }
};

struct DecodedChunk {
    // 仅非 baseline 状态（local 索引 y<<8|z<<4|x → 状态）
    std::vector<std::pair<std::uint32_t, std::uint16_t>> edits;
    ChunkEntities entities;
};

// 1.12.2 Anvil 区块编码：Level{Sections{Y,Blocks,Data[,Add]}, TileEntities}
// edits 为空且无实体时返回错误（调用方不该为纯超平坦区块写盘）
[[nodiscard]] Result<Bytes> encode_chunk(ChunkPos pos,
                                         std::span<const std::pair<std::uint32_t, std::uint16_t>> edits,
                                         const ChunkEntities& entities);

[[nodiscard]] Result<DecodedChunk> decode_chunk(ByteSpan nbt);

} // namespace cyane::world