#pragma once

#include <cstdint>
#include <memory>
#include <unordered_map>

#include "cyane/core/error.hpp"
#include "cyane/world/chunk.hpp"

namespace cyane::world {

class World {
public:
    World();
    ~World();

    // Load chunk data from the world directory
    Result<void> load_chunks();

    // Get or create a chunk at the given position
    std::shared_ptr<Chunk> get_or_create_chunk(ChunkPos pos);

    // Load chunk from anvil file if it exists
    Result<std::shared_ptr<Chunk>> load_chunk_from_anvil(ChunkPos pos);

private:
    std::unordered_map<ChunkPos, std::shared_ptr<Chunk>> chunks_;
    std::string world_dir_;
};

}