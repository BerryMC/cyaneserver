#pragma once

#include <cmath>
#include <cstdint>
#include "cyane/world/blocks.hpp"
#include "cyane/world/chunk.hpp"
#include "cyane/world/noise.hpp"

namespace cyane::world::gen {

// 1.12.2 原版矿石生成器（WorldGenMinable 逐行转写）
class WorldGenMinable {
public:
    constexpr WorldGenMinable(std::uint16_t ore_state, int block_count) noexcept
        : ore_state_(ore_state), number_of_blocks_(block_count) {}

    bool generate(Chunk& chunk, JavaRandom& rand, int x, int y, int z) const noexcept {
        constexpr double kPi = 3.14159265358979323846;
        const float f = rand.next_float() * static_cast<float>(kPi);
        const double d0 = static_cast<double>(static_cast<float>(x) + std::sin(f) * static_cast<float>(number_of_blocks_) / 8.0f);
        const double d1 = static_cast<double>(static_cast<float>(x) - std::sin(f) * static_cast<float>(number_of_blocks_) / 8.0f);
        const double d2 = static_cast<double>(static_cast<float>(z) + std::cos(f) * static_cast<float>(number_of_blocks_) / 8.0f);
        const double d3 = static_cast<double>(static_cast<float>(z) - std::cos(f) * static_cast<float>(number_of_blocks_) / 8.0f);
        const double d4 = static_cast<double>(y + rand.next_int(3) - 2);
        const double d5 = static_cast<double>(y + rand.next_int(3) - 2);

        for (int i = 0; i < number_of_blocks_; ++i) {
            const float progress = static_cast<float>(i) / static_cast<float>(number_of_blocks_);
            const double cx = d0 + (d1 - d0) * static_cast<double>(progress);
            const double cy = d4 + (d5 - d4) * static_cast<double>(progress);
            const double cz = d2 + (d3 - d2) * static_cast<double>(progress);
            const double size_factor = rand.next_double() * static_cast<double>(number_of_blocks_) / 16.0;
            const double rad_h = (std::sin(static_cast<double>(progress) * kPi) + 1.0) * size_factor + 1.0;
            const double rad_v = (std::sin(static_cast<double>(progress) * kPi) + 1.0) * size_factor + 1.0;

            const int min_bx = static_cast<int>(std::floor(cx - rad_h / 2.0));
            const int max_bx = static_cast<int>(std::floor(cx + rad_h / 2.0));
            const int min_by = static_cast<int>(std::floor(cy - rad_v / 2.0));
            const int max_by = static_cast<int>(std::floor(cy + rad_v / 2.0));
            const int min_bz = static_cast<int>(std::floor(cz - rad_h / 2.0));
            const int max_bz = static_cast<int>(std::floor(cz + rad_h / 2.0));

            for (int bx = min_bx; bx <= max_bx; ++bx) {
                const double dist_x = (static_cast<double>(bx) + 0.5 - cx) / (rad_h / 2.0);
                if (dist_x * dist_x < 1.0 && bx >= 0 && bx < 16) {
                    for (int by = min_by; by <= max_by; ++by) {
                        const double dist_y = (static_cast<double>(by) + 0.5 - cy) / (rad_v / 2.0);
                        if (dist_x * dist_x + dist_y * dist_y < 1.0 && by >= 0 && by < 256) {
                            for (int bz = min_bz; bz <= max_bz; ++bz) {
                                const double dist_z = (static_cast<double>(bz) + 0.5 - cz) / (rad_h / 2.0);
                                if (dist_x * dist_x + dist_y * dist_y + dist_z * dist_z < 1.0 && bz >= 0 && bz < 16) {
                                    const auto current = chunk.block_at(static_cast<std::size_t>(bx), by, static_cast<std::size_t>(bz));
                                    if (current == kStateStone || current == kStateGranite ||
                                        current == kStateDiorite || current == kStateAndesite ||
                                        current == kStateNetherrack) {
                                        chunk.set_block(static_cast<std::size_t>(bx), by, static_cast<std::size_t>(bz), ore_state_);
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
        return true;
    }

private:
    std::uint16_t ore_state_;
    int number_of_blocks_;
};

}  // namespace cyane::world::gen
