#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

#include "cyane/core/error.hpp"
#include "cyane/net/container_store.hpp"
#include "cyane/net/furnace_store.hpp"
#include "cyane/net/item_drop.hpp"
#include "cyane/world/region.hpp"
#include "cyane/world/world.hpp"

namespace cyane::game {

// 世界持久化编排：World 方块编辑 ↔ Anvil region（.mca），箱子/熔炉 ↔ TileEntities，
// 掉落物 ↔ Entities（minecraft:item）。
// 载入把磁盘内容合并进内存态；保存把编辑区块（含实体所在区块）写回。
// 线程约定：save 可在 tick 线程或停机时调用；各 store 内部自持锁。
class WorldPersistence {
public:
    WorldPersistence(world::World& world, net::ContainerStore& containers,
                     net::FurnaceStore& furnaces, net::ItemDropManager& item_drops,
                     std::string world_dir)
        : world_{world}, containers_{containers}, furnaces_{furnaces},
          item_drops_{item_drops}, world_dir_{std::move(world_dir)} {}

    // 读取 world_dir/region/*.mca；返回载入的区块数。目录不存在视为空世界。
    [[nodiscard]] Result<std::size_t> load();

    // 全量保存（只写有编辑/有实体的区块）；返回写出的区块数
    [[nodiscard]] Result<std::size_t> save();

private:
    [[nodiscard]] world::RegionFile& region(std::int32_t rx, std::int32_t rz);

    world::World& world_;
    net::ContainerStore& containers_;
    net::FurnaceStore& furnaces_;
    net::ItemDropManager& item_drops_;
    std::string world_dir_;
    std::unordered_map<std::int64_t, world::RegionFile> regions_;
};

} // namespace cyane::game