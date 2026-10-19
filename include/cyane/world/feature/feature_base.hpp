#pragma once

#include <cstdint>
#include "cyane/world/blocks.hpp"
#include "cyane/world/chunk.hpp"
#include "cyane/world/noise.hpp"

namespace cyane::world::gen {

// 1.12.2 原版特征生成器基类（WorldGenerator 对齐）
class WorldGenerator {
public:
    virtual ~WorldGenerator() = default;
    virtual bool generate(Chunk& chunk, JavaRandom& rand, int x, int y, int z) = 0;
};

// 1.12.2 原版巨树基类（WorldGenHugeTrees 逐行转写）
class WorldGenHugeTrees : public WorldGenerator {
public:
    constexpr WorldGenHugeTrees(int base_height, int extra_random_height, std::uint16_t wood, std::uint16_t leaves) noexcept
        : base_height_(base_height), extra_random_height_(extra_random_height), wood_state_(wood), leaves_state_(leaves) {}

protected:
    [[nodiscard]] int get_height(JavaRandom& rand) const noexcept {
        return rand.next_int(extra_random_height_ + 1) + base_height_;
    }

    int base_height_;
    int extra_random_height_;
    std::uint16_t wood_state_;
    std::uint16_t leaves_state_;
};

}  // namespace cyane::world::gen
