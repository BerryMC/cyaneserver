#include "cyane/game/world_persistence.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <map>
#include <set>
#include <utility>

#include "cyane/core/log.hpp"
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

world::RegionFile& WorldPersistence::region(std::int32_t rx, std::int32_t rz) {
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
                if (!decoded->edits.empty()) {
                    world_.merge_edits(pos, decoded->edits);
                }
                for (const auto& [key, chest] : decoded->entities.chests) {
                    containers_.ensure(key);
                    for (std::size_t slot = 0; slot < chest.size(); ++slot) {
                        containers_.set_slot(key, slot, chest[slot]);
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
                if (!decoded->edits.empty() || !decoded->entities.empty()) {
                    ++loaded;
                }
            }
        }
        // 已读文件入缓存，保存时直接覆写
        regions_.insert_or_assign(region_key(rx, rz), std::move(*file));
    }
    if (ec) {
        return make_error(ErrorCode::io, "cannot list region dir: " + ec.message());
    }
    return loaded;
}

Result<std::size_t> WorldPersistence::save() {
    struct ChunkData {
        std::vector<std::pair<std::uint32_t, std::uint16_t>> edits;
        world::ChunkEntities entities;
    };
    std::map<std::pair<std::int32_t, std::int32_t>, ChunkData> chunks;

    for (const auto& pos : world_.edited_chunks()) {
        chunks[{pos.x, pos.z}].edits = world_.chunk_edits(pos);
    }
    for (const auto& [key, chest] : containers_.all()) {
        const auto [bx, by, bz] = world::unpack_block_pos(key);
        const auto pos = world::ChunkPos::from_world(bx, bz);
        if (!pos) {
            continue;
        }
        chunks[{pos->x, pos->z}].entities.chests.emplace_back(key, chest);
    }
    for (const auto& [key, furnace] : furnaces_.all()) {
        const auto [bx, by, bz] = world::unpack_block_pos(key);
        const auto pos = world::ChunkPos::from_world(bx, bz);
        if (!pos) {
            continue;
        }
        chunks[{pos->x, pos->z}].entities.furnaces.emplace_back(key, to_stored(furnace));
    }

    std::size_t written = 0;
    for (const auto& [cpos, data] : chunks) {
        if (data.edits.empty() && data.entities.empty()) {
            continue;
        }
        const world::ChunkPos pos{cpos.first, cpos.second};
        auto encoded = world::encode_chunk(pos, data.edits, data.entities);
        if (!encoded) {
            log::warn("world: cannot encode chunk ({},{}): {}", pos.x, pos.z,
                      encoded.error().message);
            continue;
        }
        const auto rx = floor_div(pos.x, kRegionChunks);
        const auto rz = floor_div(pos.z, kRegionChunks);
        auto& file = region(rx, rz);
        auto wrote = file.write_chunk(floor_mod(pos.x, kRegionChunks),
                                      floor_mod(pos.z, kRegionChunks), ByteSpan{*encoded});
        if (!wrote) {
            log::warn("world: cannot write chunk ({},{}) to region r.{}.{}: {}", pos.x, pos.z, rx,
                      rz, wrote.error().message);
            continue;
        }
        ++written;
    }

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