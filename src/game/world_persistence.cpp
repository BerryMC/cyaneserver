#include "cyane/game/world_persistence.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <map>
#include <set>
#include <utility>

#include "cyane/core/log.hpp"
#include "cyane/net/mob_manager.hpp"
#include "cyane/world/anvil.hpp"
#include "cyane/world/blocks.hpp"

namespace cyane::game {

namespace {

constexpr int kRegionChunks = world::RegionFile::kRegionSize;

[[nodiscard]] std::int64_t region_key(std::int32_t rx, std::int32_t rz) {
    return (static_cast<std::int64_t>(rx) << 32) | static_cast<std::uint32_t>(rz);
}

[[nodiscard]] std::int32_t floor_div(std::int32_t value, std::int32_t divisor) noexcept {
    const auto q = value / divisor;
    return (value % divisor != 0 && (value < 0) != (divisor < 0)) ? q - 1 : q;
}

[[nodiscard]] std::int32_t floor_mod(std::int32_t value, std::int32_t divisor) noexcept {
    return value - floor_div(value, divisor) * divisor;
}

[[nodiscard]] world::StoredFurnace to_stored(const net::FurnaceState& state) {
    world::StoredFurnace out;
    out.input = state.input;
    out.fuel = state.fuel;
    out.output = state.output;
    out.burn_left = state.burn_left;
    out.burn_total = state.burn_total;
    out.cook_time = state.cook_time;
    return out;
}

} // namespace

// 前置条件：调用方已持有 cache_mutex_（region() 的加锁包装见下）
world::RegionFile& WorldPersistence::region_locked(std::int32_t rx, std::int32_t rz) {
    const auto key = region_key(rx, rz);
    if (auto it = regions_.find(key); it != regions_.end()) {
        return it->second;
    }
    auto loaded =
        world::RegionFile::load(world::RegionFile::path_for(world_dir_ + "/region", rx, rz));
    if (!loaded) {
        // 读不了就当空 region，稍后保存会覆盖
        log::warn("world region r.{}.{} unreadable: {}", rx, rz, loaded.error().message);
        return regions_.emplace(key, world::RegionFile{}).first->second;
    }
    return regions_.emplace(key, std::move(*loaded)).first->second;
}

world::RegionFile& WorldPersistence::region(std::int32_t rx, std::int32_t rz) {
    std::lock_guard<std::mutex> lock{cache_mutex_};
    return region_locked(rx, rz);
}

void WorldPersistence::attach_loader() {
    world_.set_loader([this](world::ChunkPos pos, world::Chunk& out, Bytes& source) -> bool {
        const auto rx = world::ChunkPos::floor_div(pos.x, kRegionChunks);
        const auto rz = world::ChunkPos::floor_div(pos.z, kRegionChunks);
        const auto cx = floor_mod(pos.x, kRegionChunks);
        const auto cz = floor_mod(pos.z, kRegionChunks);

        world::RegionFile* file = nullptr;
        std::optional<Bytes> payload;
        {
            // 持缓存锁完成"取 region + 读记录"（save 线程可能正在覆写同文件）
            std::lock_guard<std::mutex> lock{cache_mutex_};
            const auto key = region_key(rx, rz);
            if (auto it = regions_.find(key); it != regions_.end()) {
                file = &it->second;
            } else {
                auto loaded =
                    world::RegionFile::load(world::RegionFile::path_for(world_dir_ + "/region",
                                                                        rx, rz));
                if (!loaded) {
                    return false;
                }
                file = &regions_.emplace(key, std::move(*loaded)).first->second;
            }
            auto read = file->read_chunk(cx, cz);
            if (!read) {
                return false;
            }
            payload = std::move(*read);
        }
        if (!payload) {
            return false;  // 无记录：交给调用方物化
        }
        auto decoded = world::decode_chunk(ByteSpan{*payload});
        if (!decoded) {
            log::warn("world: on-demand load of chunk ({},{}) failed: {}", pos.x, pos.z,
                      decoded.error().message);
            return false;
        }
        // 方块实体：容器/熔炉以内存态优先（运行时改动不被磁盘旧值覆盖）
        for (const auto& [key, chest] : decoded->entities.chests) {
            if (!containers_.exists(key)) {
                containers_.ensure(key);
                for (std::size_t slot = 0; slot < chest.size(); ++slot) {
                    containers_.set_slot(key, slot, chest[slot]);
                }
            }
        }
        for (const auto& [key, small] : decoded->entities.small_containers) {
            if (!containers_.small_exists(key)) {
                const auto kind = static_cast<net::ContainerStore::SmallKind>(small.kind);
                containers_.ensure_small(key, kind);
                const std::size_t limit =
                    kind == net::ContainerStore::SmallKind::hopper
                        ? net::ContainerStore::kHopperSlots
                        : net::ContainerStore::kSmallSlots;
                for (std::size_t slot = 0; slot < limit; ++slot) {
                    containers_.set_small_slot(key, slot, small.slots[slot]);
                }
            }
        }
        for (const auto& [key, furnace] : decoded->entities.furnaces) {
            if (!furnaces_.exists(key)) {
                net::FurnaceState state;
                state.input = furnace.input;
                state.fuel = furnace.fuel;
                state.output = furnace.output;
                state.burn_left = furnace.burn_left;
                state.burn_total = furnace.burn_total;
                state.cook_time = furnace.cook_time;
                furnaces_.restore(key, std::move(state));
            }
        }
        out = std::move(decoded->chunk);
        source = std::move(*payload);
        return true;
    });
}

Result<std::size_t> WorldPersistence::load() {
    const std::filesystem::path region_dir{world_dir_ + "/region"};
    std::error_code ec;
    if (!std::filesystem::exists(region_dir, ec)) {
        return std::size_t{0};
    }
    std::size_t loaded = 0;
    for (const auto& entry : std::filesystem::directory_iterator{region_dir, ec}) {
        if (!entry.is_regular_file()) {
            continue;
        }
        const auto stem = entry.path().stem().string();
        std::int32_t rx = 0, rz = 0;
        if (std::sscanf(stem.c_str(), "r.%d.%d", &rx, &rz) != 2) {
            continue;
        }
        auto file = world::RegionFile::load(entry.path());
        if (!file) {
            log::warn("world: skipping {}: {}", entry.path().string(), file.error().message);
            continue;
        }
        for (int cx = 0; cx < kRegionChunks; ++cx) {
            for (int cz = 0; cz < kRegionChunks; ++cz) {
                auto chunk = file->read_chunk(cx, cz);
                if (!chunk) {
                    log::warn("world: {}: chunk ({},{}) unreadable: {}",
                              entry.path().string(), cx, cz, chunk.error().message);
                    continue;
                }
                if (!*chunk) {
                    continue;
                }
                auto decoded = world::decode_chunk(ByteSpan{**chunk});
                if (!decoded) {
                    log::warn("world: {}: chunk ({},{}) decode failed: {}",
                              entry.path().string(), cx, cz, decoded.error().message);
                    continue;
                }
                const world::ChunkPos pos{rx * kRegionChunks + cx, rz * kRegionChunks + cz};
                const bool has_content =
                    !decoded->chunk.sections().empty() || !decoded->entities.empty();
                world_.load_chunk(pos, std::move(decoded->chunk), /*dirty=*/false,
                                  std::move(**chunk));
                for (const auto& [key, chest] : decoded->entities.chests) {
                    containers_.ensure(key);
                    for (std::size_t slot = 0; slot < chest.size(); ++slot) {
                        containers_.set_slot(key, slot, chest[slot]);
                    }
                }
                for (const auto& [key, small] : decoded->entities.small_containers) {
                    const auto kind = static_cast<net::ContainerStore::SmallKind>(small.kind);
                    containers_.ensure_small(key, kind);
                    const std::size_t limit =
                        kind == net::ContainerStore::SmallKind::hopper
                            ? net::ContainerStore::kHopperSlots
                            : net::ContainerStore::kSmallSlots;
                    for (std::size_t slot = 0; slot < limit; ++slot) {
                        containers_.set_small_slot(key, slot, small.slots[slot]);
                    }
                }
                for (const auto& [key, furnace] : decoded->entities.furnaces) {
                    net::FurnaceState state;
                    state.input = furnace.input;
                    state.fuel = furnace.fuel;
                    state.output = furnace.output;
                    state.burn_left = furnace.burn_left;
                    state.burn_total = furnace.burn_total;
                    state.cook_time = furnace.cook_time;
                    furnaces_.restore(key, std::move(state));
                }
                if (!decoded->entities.items.empty()) {
                    std::vector<net::DroppedItemState> drops;
                    drops.reserve(decoded->entities.items.size());
                    for (const auto& item : decoded->entities.items) {
                        drops.push_back(net::DroppedItemState{item.x, item.y, item.z, item.stack});
                    }
                    item_drops_.restore(drops);
                    drop_chunks_.emplace(pos.x, pos.z);
                }
                if (!decoded->entities.mobs.empty()) {
                    std::vector<net::MobState> restored_mobs;
                    restored_mobs.reserve(decoded->entities.mobs.size());
                    for (const auto& mob : decoded->entities.mobs) {
                        restored_mobs.push_back(
                            net::MobState{mob.type, mob.x, mob.y, mob.z, mob.yaw, mob.pitch,
                                          mob.health});
                    }
                    mobs_.restore(restored_mobs);
                    mob_chunks_.emplace(pos.x, pos.z);
                }
                if (has_content) {
                    ++loaded;
                }
            }
        }
        // 已读文件入缓存，保存时直接覆写
        {
            std::lock_guard<std::mutex> lock{cache_mutex_};
            regions_.insert_or_assign(region_key(rx, rz), std::move(*file));
        }
    }
    if (ec) {
        return make_error(ErrorCode::io, "cannot list region dir: " + ec.message());
    }
    return loaded;
}

bool WorldPersistence::needs_save() const {
    if (!world_.dirty_chunks().empty()) {
        return true;
    }
    std::lock_guard<std::mutex> lock{cache_mutex_};
    for (const auto& [key, file] : regions_) {
        if (file.dirty()) {
            return true;
        }
    }
    return false;
}

Result<std::size_t> WorldPersistence::save() {
    // 写出集合 = 脏区块 ∪ 有方块实体的区块（箱子内容变更不一定伴随方块编辑）
    struct ChunkData {
        world::ChunkEntities entities;
    };
    std::map<std::pair<std::int32_t, std::int32_t>, ChunkData> chunks;
    std::set<std::pair<std::int32_t, std::int32_t>> write_set;

    for (const auto& pos : world_.dirty_chunks()) {
        write_set.emplace(pos.x, pos.z);
    }
    for (const auto& [key, chest] : containers_.all()) {
        const auto [bx, by, bz] = world::unpack_block_pos(key);
        const auto pos = world::ChunkPos::from_world(bx, bz);
        if (!pos) {
            continue;
        }
        write_set.emplace(pos->x, pos->z);
        chunks[{pos->x, pos->z}].entities.chests.emplace_back(key, chest);
    }
    for (const auto& [key, small] : containers_.all_small()) {
        const auto [bx, by, bz] = world::unpack_block_pos(key);
        const auto pos = world::ChunkPos::from_world(bx, bz);
        if (!pos) {
            continue;
        }
        write_set.emplace(pos->x, pos->z);
        world::StoredSmallContainer stored;
        stored.kind = static_cast<std::uint8_t>(small.kind);
        stored.slots = small.slots;
        chunks[{pos->x, pos->z}].entities.small_containers.emplace_back(key, std::move(stored));
    }
    for (const auto& [key, furnace] : furnaces_.all()) {
        const auto [bx, by, bz] = world::unpack_block_pos(key);
        const auto pos = world::ChunkPos::from_world(bx, bz);
        if (!pos) {
            continue;
        }
        write_set.emplace(pos->x, pos->z);
        chunks[{pos->x, pos->z}].entities.furnaces.emplace_back(key, to_stored(furnace));
    }

    // 掉落物按整方块坐标归入区块（floor_div 处理负坐标）
    const auto drops = item_drops_.all_drops();
    for (const auto& drop : drops) {
        const auto pos =
            world::ChunkPos::from_world(static_cast<std::int32_t>(std::floor(drop.x)),
                                        static_cast<std::int32_t>(std::floor(drop.z)));
        if (!pos) {
            continue;
        }
        write_set.emplace(pos->x, pos->z);
        drop_chunks_.emplace(pos->x, pos->z);
        chunks[{pos->x, pos->z}].entities.items.push_back(
            world::StoredEntity{drop.x, drop.y, drop.z, drop.stack});
    }

    // 生物按所在区块归组，与掉落物同法重写 Entities 列表
    const auto current_mobs = mobs_.all_mobs();
    for (const auto& mob : current_mobs) {
        const auto pos =
            world::ChunkPos::from_world(static_cast<std::int32_t>(std::floor(mob.x)),
                                        static_cast<std::int32_t>(std::floor(mob.z)));
        if (!pos) {
            continue;
        }
        write_set.emplace(pos->x, pos->z);
        mob_chunks_.emplace(pos->x, pos->z);
        chunks[{pos->x, pos->z}].entities.mobs.push_back(
            world::StoredMob{mob.type, mob.x, mob.y, mob.z, mob.yaw, mob.pitch, mob.health});
    }

    // 实体曾存在的区块无条件重写：拾取/漫游离开后清除磁盘旧副本
    write_set.insert(drop_chunks_.begin(), drop_chunks_.end());
    write_set.insert(mob_chunks_.begin(), mob_chunks_.end());

    static const world::ChunkEntities kNoEntities{};
    std::size_t written = 0;
    for (const auto& cpos : write_set) {
        const world::ChunkPos pos{cpos.first, cpos.second};
        const auto data_it = chunks.find(cpos);
        const auto& entities =
            data_it != chunks.end() ? data_it->second.entities : kNoEntities;

        // 区块不在内存 = 已按视距释放。此时 chunk_at 会物化出超平坦假区块，
        // 若拿它编码会把真实地形覆盖成超平坦（历史数据丢失事故的根源）。
        // 改走"仅实体合并"：以 region 缓存里的磁盘原 NBT 为底，只更新
        // TileEntities/Entities，方块数据原样保留。
        const bool in_memory = world_.contains(pos);
        auto encoded = [&]() -> Result<Bytes> {
            if (in_memory) {
                const auto chunk = world_.chunk_at(pos);
                const auto source = world_.source_nbt(pos);
                return source.empty()
                           ? world::encode_chunk(pos, chunk, entities)
                           : world::encode_chunk_merged(pos, chunk, entities, ByteSpan{source});
            }
            const auto rx0 = floor_div(pos.x, kRegionChunks);
            const auto rz0 = floor_div(pos.z, kRegionChunks);
            auto& file = region(rx0, rz0);
            auto source = file.read_chunk(floor_mod(pos.x, kRegionChunks),
                                          floor_mod(pos.z, kRegionChunks));
            if (!source || !*source) {
                // region 里也没有记录：新世界的未加载区块，无地形可保，整块编码
                return world::encode_chunk(pos, world::Chunk{pos}, entities);
            }
            return world::encode_chunk_entities_only(pos, entities, ByteSpan{**source});
        }();
        if (!encoded) {
            log::warn("world: cannot encode chunk ({},{}): {}", pos.x, pos.z,
                      encoded.error().message);
            continue;
        }
        const auto rx = floor_div(pos.x, kRegionChunks);
        const auto rz = floor_div(pos.z, kRegionChunks);
        // write_chunk 覆写 region 内存镜像，需与按需 loader 的 read_chunk 互斥
        Result<bool> wrote;
        {
            std::lock_guard<std::mutex> lock{cache_mutex_};
            auto& file = region_locked(rx, rz);
            wrote = file.write_chunk(floor_mod(pos.x, kRegionChunks),
                                     floor_mod(pos.z, kRegionChunks), ByteSpan{*encoded});
        }
        if (!wrote) {
            log::warn("world: cannot write chunk ({},{}) to region r.{}.{}: {}", pos.x, pos.z, rx,
                      rz, wrote.error().message);
            continue;
        }
        world_.clear_dirty(pos);
        ++written;
    }

    std::lock_guard<std::mutex> flush_lock{cache_mutex_};
    for (auto& [key, file] : regions_) {
        if (!file.dirty()) {
            continue;
        }
        const auto rx = static_cast<std::int32_t>(key >> 32);
        const auto rz = static_cast<std::int32_t>(static_cast<std::uint32_t>(key & 0xFFFFFFFFll));
        if (auto saved =
                file.save(world::RegionFile::path_for(world_dir_ + "/region", rx, rz));
            !saved) {
            log::warn("world: cannot save region r.{}.{}: {}", rx, rz, saved.error().message);
        }
    }
    return written;
}

} // namespace cyane::game