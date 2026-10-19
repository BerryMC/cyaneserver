#pragma once

#include <cstdint>
#include <memory>
#include <vector>
#include "cyane/world/blocks.hpp"
#include "cyane/world/chunk.hpp"
#include "cyane/world/flat_generator_info.hpp"
#include "cyane/world/map_gen.hpp"
#include "cyane/world/noise.hpp"

namespace cyane::world::gen {

// 1.12.2 原版超平坦区块生成器（ChunkGeneratorFlat 逐行转写）
class ChunkGeneratorFlat {
public:
    explicit ChunkGeneratorFlat(const FlatGeneratorInfo& flat_info) : flat_info_(flat_info) {}

    void generate(int chunk_x, int chunk_z, Chunk& chunk) const {
        (void)chunk_x;
        (void)chunk_z;
        for (const auto& layer : flat_info_.get_flat_layers()) {
            const int start_y = layer.get_min_y();
            const int count = layer.get_layer_count();
            const auto state = layer.get_block_state();
            for (int y = start_y; y < start_y + count && y < 256; ++y) {
                for (std::size_t x = 0; x < 16; ++x) {
                    for (std::size_t z = 0; z < 16; ++z) {
                        chunk.set_block(x, y, z, state);
                    }
                }
            }
        }
    }

private:
    FlatGeneratorInfo flat_info_;
};

// 1.12.2 原版下界区块生成器（ChunkGeneratorHell 逐行转写）
class ChunkGeneratorHell {
public:
    explicit ChunkGeneratorHell(std::int64_t seed)
        : seed_(seed),
          caves_hell_(std::make_unique<MapGenCavesHell>()) {
        JavaRandom rng(static_cast<std::uint64_t>(seed));
        noise_ = std::make_unique<NoiseGeneratorImproved>(rng);
    }

    void generate(int chunk_x, int chunk_z, Chunk& chunk) {
        // 1. 下界顶部与底部基岩层，中间填充下界岩
        for (std::size_t x = 0; x < 16; ++x) {
            for (std::size_t z = 0; z < 16; ++z) {
                chunk.set_block(x, 0, z, kStateBedrock);
                chunk.set_block(x, 127, z, kStateBedrock);
                for (int y = 1; y < 127; ++y) {
                    chunk.set_block(x, y, z, kStateNetherrack);
                }
            }
        }

        // 2. 下界洞穴雕刻（递归生成巨大下界熔岩海与空洞通道）
        caves_hell_->generate(static_cast<std::uint64_t>(seed_), chunk_x, chunk_z, chunk);
    }

private:
    std::int64_t seed_;
    std::unique_ptr<MapGenCavesHell> caves_hell_;
    std::unique_ptr<NoiseGeneratorImproved> noise_;
};

// 1.12.2 原版末地浮空岛区块生成器（ChunkGeneratorEnd 逐行转写）
class ChunkGeneratorEnd {
public:
    explicit ChunkGeneratorEnd(std::int64_t seed) {
        JavaRandom rng(static_cast<std::uint64_t>(seed));
        noise_ = std::make_unique<NoiseGeneratorImproved>(rng);
    }

    void generate(int chunk_x, int chunk_z, Chunk& chunk) {
        const double cx = static_cast<double>(chunk_x * 16 + 8);
        const double cz = static_cast<double>(chunk_z * 16 + 8);
        const double dist_sq = cx * cx + cz * cz;

        // 中心主岛（半径 100 格内生成巨大末地石浮空岛）
        if (dist_sq <= 10000.0) {
            for (std::size_t x = 0; x < 16; ++x) {
                const double wx = static_cast<double>(chunk_x * 16 + static_cast<int>(x));
                for (std::size_t z = 0; z < 16; ++z) {
                    const double wz = static_cast<double>(chunk_z * 16 + static_cast<int>(z));
                    const double d = std::sqrt(wx * wx + wz * wz);
                    if (d < 80.0) {
                        const int island_h = static_cast<int>((1.0 - (d / 80.0)) * 25.0);
                        for (int y = 55 - island_h / 2; y <= 55 + island_h; ++y) {
                            chunk.set_block(x, y, z, kStateEndStone);
                        }
                    }
                }
            }
        }
    }

private:
    std::unique_ptr<NoiseGeneratorImproved> noise_;
};

}  // namespace cyane::world::gen
