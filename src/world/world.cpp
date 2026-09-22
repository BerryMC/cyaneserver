#include "cyane/world/world.hpp"

#include <filesystem>
#include <fstream>

#include "cyane/world/region.hpp"

namespace cyane::world {

World::World() = default;
World::~World() = default;

Result<void> World::load_chunks() {
    // TODO: Implement chunk loading from disk
    return {};
}

std::shared_ptr<Chunk> World::get_or_create_chunk(ChunkPos pos) {
    auto it = chunks_.find(pos);
    if (it != chunks_.end()) {
        return it->second;
    }
    auto chunk = std::make_shared<Chunk>(pos);
    chunks_[pos] = chunk;
    return chunk;
}

Result<std::shared_ptr<Chunk>> World::load_chunk_from_anvil(ChunkPos pos) {
    // Try to load from region file
    const std::filesystem::path region_path =
        std::filesystem::path{world_dir_} / "region" /
        std::format("r.{}.{}.mca", pos.x / RegionFile::kRegionSize, pos.z / RegionFile::kRegionSize);

    if (!std::filesystem::exists(region_path)) {
        // No region file, return empty chunk
        auto chunk = std::make_shared<Chunk>(pos);
        return chunk;
    }

    auto region = RegionFile::load(region_path.string());
    if (!region) {
        return std::unexpected{std::move(region.error())};
    }

    const int local_cx = pos.x % RegionFile::kRegionSize;
    const int local_cz = pos.z % RegionFile::kRegionSize;

    if (!region->has_chunk(local_cx, local_cz)) {
        // Chunk not present in region file
        auto chunk = std::make_shared<Chunk>(pos);
        return chunk;
    }

    auto chunk_data = region->chunk_data(local_cx, local_cz);
    if (!chunk_data) {
        auto chunk = std::make_shared<Chunk>(pos);
        return chunk;
    }

    // Parse NBT data from chunk_data to populate the chunk
    // For now, return an empty chunk (NBT parsing would go here)
    auto chunk = std::make_shared<Chunk>(pos);
    return chunk;
}

}