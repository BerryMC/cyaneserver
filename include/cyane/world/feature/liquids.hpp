#pragma once

#include <array>
#include <cstdlib>
#include "cyane/world/blocks.hpp"
#include "cyane/world/chunk.hpp"
#include "cyane/world/noise.hpp"

namespace cyane::world::gen {

// 1.12.2 原版湖泊生成器（WorldGenLakes 逐行转写）
class WorldGenLakes {
public:
    explicit constexpr WorldGenLakes(std::uint16_t liquid_state) noexcept : liquid_(liquid_state) {}

    bool generate(Chunk& chunk, JavaRandom& rand, int x, int y, int z) const noexcept {
        if (y <= 4 || y >= 250) return false;

        std::array<bool, 2048> cavity{};
        const int num_spheres = rand.next_int(4) + 4;
        for (int i = 0; i < num_spheres; ++i) {
            const double rad_x = rand.next_double() * 6.0 + 3.0;
            const double rad_y = rand.next_double() * 4.0 + 2.0;
            const double rad_z = rand.next_double() * 6.0 + 3.0;
            const double cx = rand.next_double() * (16.0 - rad_x - 2.0) + 1.0 + rad_x / 2.0;
            const double cy = rand.next_double() * (8.0 - rad_y - 4.0) + 2.0 + rad_y / 2.0;
            const double cz = rand.next_double() * (16.0 - rad_z - 2.0) + 1.0 + rad_z / 2.0;

            for (int lx = 1; lx < 15; ++lx) {
                for (int lz = 1; lz < 15; ++lz) {
                    for (int ly = 1; ly < 7; ++ly) {
                        const double ddx = (static_cast<double>(lx) - cx) / (rad_x / 2.0);
                        const double ddy = (static_cast<double>(ly) - cy) / (rad_y / 2.0);
                        const double ddz = (static_cast<double>(lz) - cz) / (rad_z / 2.0);
                        if (ddx * ddx + ddy * ddy + ddz * ddz < 1.0) {
                            cavity[static_cast<std::size_t>((lx * 16 + lz) * 8 + ly)] = true;
                        }
                    }
                }
            }
        }

        for (int lx = 0; lx < 16; ++lx) {
            const int bx = x + lx - 8;
            for (int lz = 0; lz < 16; ++lz) {
                const int bz = z + lz - 8;
                for (int ly = 0; ly < 8; ++ly) {
                    const int by = y + ly - 4;
                    const auto c_idx = static_cast<std::size_t>((lx * 16 + lz) * 8 + ly);
                    const bool is_border = !cavity[c_idx] && (
                        (lx < 15 && cavity[c_idx + 128]) || (lx > 0 && cavity[c_idx - 128]) ||
                        (lz < 15 && cavity[c_idx + 8]) || (lz > 0 && cavity[c_idx - 8]) ||
                        (ly < 7 && cavity[c_idx + 1]) || (ly > 0 && cavity[c_idx - 1]));
                    if (is_border && bx >= 0 && bx < 16 && bz >= 0 && bz < 16 && by >= 1 && by < 255) {
                        const auto block = chunk.block_at(static_cast<std::size_t>(bx), by, static_cast<std::size_t>(bz));
                        if (ly >= 4 && (block == kStateWater || block == kStateLava)) {
                            return false;
                        }
                        if (ly < 4 && !is_solid(block) && block != liquid_) {
                            return false;
                        }
                    }
                }
            }
        }

        for (int lx = 0; lx < 16; ++lx) {
            const int bx = x + lx - 8;
            if (bx < 0 || bx >= 16) continue;
            for (int lz = 0; lz < 16; ++lz) {
                const int bz = z + lz - 8;
                if (bz < 0 || bz >= 16) continue;
                for (int ly = 0; ly < 8; ++ly) {
                    const int by = y + ly - 4;
                    if (by < 1 || by >= 255) continue;
                    if (cavity[static_cast<std::size_t>((lx * 16 + lz) * 8 + ly)]) {
                        chunk.set_block(static_cast<std::size_t>(bx), by, static_cast<std::size_t>(bz),
                                        ly < 4 ? liquid_ : kStateAir);
                    }
                }
            }
        }
        return true;
    }

private:
    std::uint16_t liquid_;
};

// 1.12.2 原版岩石泉眼生成器（WorldGenLiquids 逐行转写）
class WorldGenLiquids {
public:
    explicit constexpr WorldGenLiquids(std::uint16_t liquid_state) noexcept : liquid_(liquid_state) {}

    bool generate(Chunk& chunk, int x, int y, int z) const noexcept {
        if (y < 5 || y > 250 || x <= 0 || x >= 15 || z <= 0 || z >= 15) return false;
        const auto sx = static_cast<std::size_t>(x);
        const auto sz = static_cast<std::size_t>(z);

        if (chunk.block_at(sx, y + 1, sz) != kStateStone || chunk.block_at(sx, y - 1, sz) != kStateStone) {
            return false;
        }
        if (chunk.block_at(sx, y, sz) != kStateStone && chunk.block_at(sx, y, sz) != kStateAir) {
            return false;
        }

        int stone_sides = 0;
        int air_sides = 0;
        if (chunk.block_at(sx - 1, y, sz) == kStateStone) ++stone_sides;
        else if (chunk.block_at(sx - 1, y, sz) == kStateAir) ++air_sides;

        if (chunk.block_at(sx + 1, y, sz) == kStateStone) ++stone_sides;
        else if (chunk.block_at(sx + 1, y, sz) == kStateAir) ++air_sides;

        if (chunk.block_at(sx, y, sz - 1) == kStateStone) ++stone_sides;
        else if (chunk.block_at(sx, y, sz - 1) == kStateAir) ++air_sides;

        if (chunk.block_at(sx, y, sz + 1) == kStateStone) ++stone_sides;
        else if (chunk.block_at(sx, y, sz + 1) == kStateAir) ++air_sides;

        if (stone_sides == 3 && air_sides == 1) {
            chunk.set_block(sx, y, sz, liquid_);
            return true;
        }
        return false;
    }

private:
    std::uint16_t liquid_;
};

// 1.12.2 原版水底黏土盘生成器（WorldGenClay）
class WorldGenClay {
public:
    static bool generate(Chunk& chunk, JavaRandom& rand, int x, int y, int z) noexcept {
        if (chunk.block_at(static_cast<std::size_t>(x), y, static_cast<std::size_t>(z)) != kStateWater) {
            return false;
        }
        const int radius = rand.next_int(2) + 2;
        for (int cx = x - radius; cx <= x + radius; ++cx) {
            const int dx = cx - x;
            for (int cz = z - radius; cz <= z + radius; ++cz) {
                const int dz = cz - z;
                if (dx * dx + dz * dz <= radius * radius) {
                    if (cx >= 0 && cx < 16 && cz >= 0 && cz < 16) {
                        for (int cy = y - 1; cy <= y; ++cy) {
                            const auto s = chunk.block_at(static_cast<std::size_t>(cx), cy, static_cast<std::size_t>(cz));
                            if (s == kStateSand || s == kStateDirt || s == kStateGravel) {
                                chunk.set_block(static_cast<std::size_t>(cx), cy, static_cast<std::size_t>(cz), kStateClay);
                            }
                        }
                    }
                }
            }
        }
        return true;
    }
};

// 1.12.2 原版水底沙盘/砾石盘生成器（WorldGenSand）
class WorldGenSand {
public:
    explicit constexpr WorldGenSand(std::uint16_t sand_state, int radius = 7) noexcept
        : state_(sand_state), radius_(radius) {}

    bool generate(Chunk& chunk, JavaRandom& rand, int x, int y, int z) const noexcept {
        if (chunk.block_at(static_cast<std::size_t>(x), y, static_cast<std::size_t>(z)) != kStateWater) {
            return false;
        }
        const int r = rand.next_int(radius_ - 2) + 2;
        for (int cx = x - r; cx <= x + r; ++cx) {
            const int dx = cx - x;
            for (int cz = z - r; cz <= z + r; ++cz) {
                const int dz = cz - z;
                if (dx * dx + dz * dz <= r * r) {
                    if (cx >= 0 && cx < 16 && cz >= 0 && cz < 16) {
                        for (int cy = y - 2; cy <= y; ++cy) {
                            const auto s = chunk.block_at(static_cast<std::size_t>(cx), cy, static_cast<std::size_t>(cz));
                            if (s == kStateDirt || s == kStateGrass) {
                                chunk.set_block(static_cast<std::size_t>(cx), cy, static_cast<std::size_t>(cz), state_);
                            }
                        }
                    }
                }
            }
        }
        return true;
    }

private:
    std::uint16_t state_;
    int radius_;
};

}  // namespace cyane::world::gen
