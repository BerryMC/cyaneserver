#pragma once

#include <cstdint>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>

#include "cyane/core/error.hpp"
#include "cyane/net/container_store.hpp"
#include "cyane/net/furnace_store.hpp"
#include "cyane/net/item_drop.hpp"
#include "cyane/net/mob_manager.hpp"
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
                     net::MobManager& mobs, std::string world_dir)
        : world_{world}, containers_{containers}, furnaces_{furnaces},
          item_drops_{item_drops}, mobs_{mobs}, world_dir_{std::move(world_dir)} {}

    // 读取 world_dir/region/*.mca；返回载入的区块数。目录不存在视为空世界。
    [[nodiscard]] Result<std::size_t> load();

    // 全量保存（只写有编辑/有实体的区块）；返回写出的区块数
    [[nodiscard]] Result<std::size_t> save();

    // 注入按需加载回调：区块被视距释放后玩家回来时，从 region 缓存重读
    // 真实地形并恢复方块实体（容器/熔炉以内存态优先，不覆盖运行时改动）。
    void attach_loader();

private:
    [[nodiscard]] world::RegionFile& region(std::int32_t rx, std::int32_t rz);
    // 前置条件：cache_mutex_ 已持有（内部包装用）
    [[nodiscard]] world::RegionFile& region_locked(std::int32_t rx, std::int32_t rz);

    world::World& world_;
    net::ContainerStore& containers_;
    net::FurnaceStore& furnaces_;
    net::ItemDropManager& item_drops_;
    net::MobManager& mobs_;
    std::string world_dir_;
    // regions_ 会被 load（启动）、save（tick/停机线程）、按需 loader（reactor
    // 线程）并发触碰，统一走 cache_mutex_。
    mutable std::mutex cache_mutex_;
    std::unordered_map<std::int64_t, world::RegionFile> regions_;
    // 曾含掉落物/生物的区块。实体在内存里会被拾取或漫游离开，原区块不再满足
    // 写出条件时磁盘上会残留旧副本（下次载入复活）——这些区块保存时无条件重写。
    std::set<std::pair<std::int32_t, std::int32_t>> drop_chunks_;
    std::set<std::pair<std::int32_t, std::int32_t>> mob_chunks_;
};

} // namespace cyane::game