#pragma once

#include "cyane/world/blocks.hpp"
#include "cyane/world/noise.hpp"
#include "cyane/world/world.hpp"

namespace cyane::world::gen {

// WorldGenDungeons.generate 的前半段（预检 + 开凿 + 墙体）。
// 房间半径 j/k1 ∈ [2,3]，纵向 y ∈ [-1,4]，四壁/地板/天花板换成圆石，
// 地板有 3/4 概率换成苔石；内部清空成空气。
//
// 尚未实现：原版后半段的箱子（2×3 次尝试）与刷怪笼放置。
[[nodiscard]] inline bool generate_dungeon(World& world, gen::JavaRandom& rand,
                                           std::int32_t ox, std::int32_t oy, std::int32_t oz) {
    const std::int32_t j = rand.next_int(2) + 2;
    const std::int32_t k = -j - 1;
    const std::int32_t l = j + 1;
    const std::int32_t k1 = rand.next_int(2) + 2;
    const std::int32_t l1 = -k1 - 1;
    const std::int32_t i2 = k1 + 1;

    std::int32_t openings = 0;
    for (std::int32_t dx = k; dx <= l; ++dx) {
        for (std::int32_t dy = -1; dy <= 4; ++dy) {
            for (std::int32_t dz = l1; dz <= i2; ++dz) {
                const auto state = world.block_at(ox + dx, oy + dy, oz + dz);
                if ((dy == -1 || dy == 4) && !is_solid(state)) {
                    return false;
                }
                if ((dx == k || dx == l || dz == l1 || dz == i2) && dy == 0 &&
                    state == kStateAir &&
                    world.block_at(ox + dx, oy + dy + 1, oz + dz) == kStateAir) {
                    ++openings;
                }
            }
        }
    }
    if (openings < 1 || openings > 5) {
        return false;
    }

    for (std::int32_t dx = k; dx <= l; ++dx) {
        for (std::int32_t dy = 3; dy >= -1; --dy) {
            for (std::int32_t dz = l1; dz <= i2; ++dz) {
                const auto wx = ox + dx;
                const auto wy = oy + dy;
                const auto wz = oz + dz;
                const auto state = world.block_at(wx, wy, wz);
                const bool interior =
                    dx != k && dy != -1 && dz != l1 && dx != l && dy != 4 && dz != i2;
                if (interior) {
                    if (state != kStateChest) {
                        world.set_block(wx, wy, wz, kStateAir);
                    }
                } else if (wy >= 0 && !is_solid(world.block_at(wx, wy - 1, wz))) {
                    world.set_block(wx, wy, wz, kStateAir);
                } else if (is_solid(state) && state != kStateChest) {
                    if (dy == -1 && rand.next_int(4) != 0) {
                        world.set_block(wx, wy, wz, kStateMossyCobblestone);
                    } else {
                        world.set_block(wx, wy, wz, kStateCobblestone);
                    }
                }
            }
        }
    }
    return true;
}

}  // namespace cyane::world::gen
