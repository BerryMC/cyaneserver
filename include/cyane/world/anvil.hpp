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

// 小容器持久化（发射器/投掷器 9 格；漏斗 5 格，数组后 4 格恒空）。
// kind：0=dispenser 1=dropper 2=hopper（与 net::ContainerStore::SmallKind 对齐，
// world 层不反向依赖 net，故用裸常量）
inline constexpr std::uint8_t kSmallKindDispenser = 0;
inline constexpr std::uint8_t kSmallKindDropper = 1;
inline constexpr std::uint8_t kSmallKindHopper = 2;
inline constexpr std::size_t kSmallPersistSlots = 9;
struct StoredSmallContainer {
    std::uint8_t kind{kSmallKindDispenser};
    std::array<item::ItemStack, kSmallPersistSlots> slots{};
};

// 掉落物品实体（region 的 Entities 列表，id=minecraft:item）
struct StoredEntity {
    double x{0.0};
    double y{0.0};
    double z{0.0};
    item::ItemStack stack;
};

// 生物实体（region 的 Entities 列表）。type 为 1.12.2 SpawnMob 类型：
// 90 猪 91 羊 92 牛 93 鸡（当前建模的被动生物集合）
struct StoredMob {
    std::uint8_t type{90};
    double x{0.0};
    double y{0.0};
    double z{0.0};
    float yaw{0.0f};
    float pitch{0.0f};
    float health{20.0f};
};

// 一个区块的方块实体（按方块位置键 pack_block_pos 索引）+ 实体
struct ChunkEntities {
    std::vector<std::pair<std::int64_t, StoredChest>> chests;
    std::vector<std::pair<std::int64_t, StoredFurnace>> furnaces;
    std::vector<std::pair<std::int64_t, StoredSmallContainer>> small_containers;
    std::vector<StoredEntity> items;
    std::vector<StoredMob> mobs;
    [[nodiscard]] bool empty() const noexcept {
        return chests.empty() && furnaces.empty() && small_containers.empty() && items.empty() &&
               mobs.empty();
    }
};

// 解码结果：完整区块（缺失 section = 空气）+ 方块实体
struct DecodedChunk {
    Chunk chunk;
    ChunkEntities entities;
};

// 1.12.2 Anvil 区块编码：Level{Sections{Y,Blocks,Data[,Add]}, TileEntities}
// 全空气 section 跳过；方块数据按存储原样写出（无损，不做 baseline 变换）
[[nodiscard]] Result<Bytes> encode_chunk(ChunkPos pos, const Chunk& chunk,
                                         const ChunkEntities& entities);

// 无损保存：以 source_nbt（磁盘原始 NBT）为底，仅替换我们管理的字段——
// 逐 section 覆盖 Blocks/Data/Add（保留 BlockLight/SkyLight），
// TileEntities 保留未建模实体后追加箱子/熔炉。其余字段（Biomes/HeightMap/
// Entities/InhabitedTime 等）原样透传。source_nbt 为空则退化为 encode_chunk。
[[nodiscard]] Result<Bytes> encode_chunk_merged(ChunkPos pos, const Chunk& chunk,
                                                const ChunkEntities& entities,
                                                ByteSpan source_nbt);

// 仅实体合并：区块不在内存（已按视距释放）但容器/实体记录命中写集时，
// 以磁盘源 NBT 为底只更新 TileEntities/Entities——方块数据原样保留，
// 绝不能用物化出的超平坦假区块覆盖真实地形。
[[nodiscard]] Result<Bytes> encode_chunk_entities_only(ChunkPos pos, const ChunkEntities& entities,
                                                       ByteSpan source_nbt);

[[nodiscard]] Result<DecodedChunk> decode_chunk(ByteSpan nbt);

} // namespace cyane::world