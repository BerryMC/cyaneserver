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
    constexpr WorldGenMinable(std::uint16_t ore_state, int block_count,
                              std::uint16_t target_state = kStateStone) noexcept
        : ore_state_(ore_state), block_count_(block_count), target_state_(target_state) {}

    bool generate(Chunk& chunk, JavaRandom& rand, int x, int y, int z) const noexcept {
        constexpr double kPi = 3.14159265358979323846;
        const double f = static_cast<double>(rand.next_float()) * kPi;
        const double d0 = static_cast<double>(x) + std::sin(f) * static_cast<double>(block_count_) / 8.0;
        const double d1 = static_cast<double>(x) - std::sin(f) * static_cast<double>(block_count_) / 8.0;
        const double d2 = static_cast<double>(z) + std::cos(f) * static_cast<double>(block_count_) / 8.0;
        const double d3 = static_cast<double>(z) - std::cos(f) * static_cast<double>(block_count_) / 8.0;
        const double d4 = static_cast<double>(y + rand.next_int(3) - 2);
        const double d5 = static_cast<double>(y + rand.next_int(3) - 2);

        for (int i = 0; i < block_count_; ++i) {
            const double f1 = static_cast<double>(i) / static_cast<double>(block_count_);
            const double d6 = d0 + (d1 - d0) * f1;
            const double d7 = d4 + (d5 - d4) * f1;
            const double d8 = d2 + (d3 - d2) * f1;
            const double d9 = rand.next_double() * static_cast<double>(block_count_) / 16.0;
            const double d10 = (std::sin(kPi * f1) + 1.0) * d9 + 1.0;
            const double d11 = (std::sin(kPi * f1) + 1.0) * d9 + 1.0;
            const int j = static_cast<int>(std::floor(d6 - d10 / 2.0));
            const int k = static_cast<int>(std::floor(d7 - d11 / 2.0));
            const int l = static_cast<int>(std::floor(d8 - d10 / 2.0));
            const int i1 = static_cast<int>(std::floor(d6 + d10 / 2.0));
            const int j1 = static_cast<int>(std::floor(d7 + d11 / 2.0));
            const int k1 = static_cast<int>(std::floor(d8 + d10 / 2.0));

            for (int l1 = j; l1 <= i1; ++l1) {
                const double d12 = (static_cast<double>(l1) + 0.5 - d6) / (d10 / 2.0);
                if (d12 * d12 < 1.0) {
                    for (int i2 = k; i2 <= j1; ++i2) {
                        const double d13 = (static_cast<double>(i2) + 0.5 - d7) / (d11 / 2.0);
                        if (d12 * d12 + d13 * d13 < 1.0) {
                            for (int j2 = l; j2 <= k1; ++j2) {
                                const double d14 = (static_cast<double>(j2) + 0.5 - d8) / (d10 / 2.0);
                                if (d12 * d12 + d13 * d13 + d14 * d14 < 1.0) {
                                    if (l1 >= 0 && l1 < 16 && j2 >= 0 && j2 < 16 && i2 >= 1 && i2 < 255) {
                                        if (chunk.block_at(static_cast<std::size_t>(l1), i2, static_cast<std::size_t>(j2)) == target_state_) {
                                            chunk.set_block(static_cast<std::size_t>(l1), i2, static_cast<std::size_t>(j2), ore_state_);
                                        }
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
    int block_count_;
    std::uint16_t target_state_;
};

// 1.12.2 原版树木生成器（WorldGenTrees 逐行转写）
class WorldGenTrees {
public:
    explicit WorldGenTrees(int min_height = 4, std::uint16_t wood = kStateOakLog,
                           std::uint16_t leaves = kStateOakLeaves) noexcept
        : min_height_(min_height), wood_state_(wood), leaves_state_(leaves) {}

    bool generate(Chunk& chunk, JavaRandom& rand, int x, int y, int z) const noexcept {
        const int height = rand.next_int(3) + min_height_;
        if (y < 1 || y + height + 1 > 255) {
            return false;
        }

        // 空间校验：检查树干与树冠范围
        for (int cy = y; cy <= y + 1 + height; ++cy) {
            int radius = 1;
            if (cy == y) radius = 0;
            if (cy >= y + 1 + height - 2) radius = 2;

            for (int cx = x - radius; cx <= x + radius; ++cx) {
                for (int cz = z - radius; cz <= z + radius; ++cz) {
                    if (cx < 0 || cx >= 16 || cz < 0 || cz >= 16 || cy >= 256) {
                        return false;
                    }
                    const auto state = chunk.block_at(static_cast<std::size_t>(cx), cy, static_cast<std::size_t>(cz));
                    if (state != kStateAir && state != kStateOakLeaves) {
                        return false;
                    }
                }
            }
        }

        // 泥土/草支撑校验
        const auto soil = chunk.block_at(static_cast<std::size_t>(x), y - 1, static_cast<std::size_t>(z));
        if (soil != kStateGrass && soil != kStateDirt) {
            return false;
        }
        chunk.set_block(static_cast<std::size_t>(x), y - 1, static_cast<std::size_t>(z), kStateDirt);

        // 生成树叶层
        for (int cy = y - 3 + height; cy <= y + height; ++cy) {
            const int offset = cy - (y + height);
            const int leaf_radius = 1 - offset / 2;

            for (int cx = x - leaf_radius; cx <= x + leaf_radius; ++cx) {
                const int dx = cx - x;
                for (int cz = z - leaf_radius; cz <= z + leaf_radius; ++cz) {
                    const int dz = cz - z;
                    if (std::abs(dx) != leaf_radius || std::abs(dz) != leaf_radius ||
                        (rand.next_int(2) != 0 && offset != 0)) {
                        if (cx >= 0 && cx < 16 && cz >= 0 && cz < 16 && cy >= 0 && cy < 256) {
                            const auto current = chunk.block_at(static_cast<std::size_t>(cx), cy, static_cast<std::size_t>(cz));
                            if (current == kStateAir) {
                                chunk.set_block(static_cast<std::size_t>(cx), cy, static_cast<std::size_t>(cz), leaves_state_);
                            }
                        }
                    }
                }
            }
        }

        // 生成树干
        for (int cy = 0; cy < height; ++cy) {
            const auto current = chunk.block_at(static_cast<std::size_t>(x), y + cy, static_cast<std::size_t>(z));
            if (current == kStateAir || current == leaves_state_) {
                chunk.set_block(static_cast<std::size_t>(x), y + cy, static_cast<std::size_t>(z), wood_state_);
            }
        }

        return true;
    }

private:
    int min_height_;
    std::uint16_t wood_state_;
    std::uint16_t leaves_state_;
};

// 1.12.2 原版地表杂草生成器（WorldGenTallGrass）
class WorldGenTallGrass {
public:
    static bool generate(Chunk& chunk, JavaRandom& rand, int x, int y, int z) noexcept {
        for (int i = 0; i < 16; ++i) {
            const int rx = x + rand.next_int(8) - rand.next_int(8);
            const int rz = z + rand.next_int(8) - rand.next_int(8);
            const int ry = y + rand.next_int(4) - rand.next_int(4);
            if (rx >= 0 && rx < 16 && rz >= 0 && rz < 16 && ry >= 1 && ry < 255) {
                if (chunk.block_at(static_cast<std::size_t>(rx), ry, static_cast<std::size_t>(rz)) == kStateAir &&
                    chunk.block_at(static_cast<std::size_t>(rx), ry - 1, static_cast<std::size_t>(rz)) == kStateGrass) {
                    chunk.set_block(static_cast<std::size_t>(rx), ry, static_cast<std::size_t>(rz), kStateTallGrass);
                }
            }
        }
        return true;
    }
};

// 1.12.2 原版地表花朵生成器（WorldGenFlowers）
class WorldGenFlowers {
public:
    static bool generate(Chunk& chunk, JavaRandom& rand, int x, int y, int z, std::uint16_t flower_state) noexcept {
        for (int i = 0; i < 16; ++i) {
            const int rx = x + rand.next_int(8) - rand.next_int(8);
            const int rz = z + rand.next_int(8) - rand.next_int(8);
            const int ry = y + rand.next_int(4) - rand.next_int(4);
            if (rx >= 0 && rx < 16 && rz >= 0 && rz < 16 && ry >= 1 && ry < 255) {
                if (chunk.block_at(static_cast<std::size_t>(rx), ry, static_cast<std::size_t>(rz)) == kStateAir &&
                    chunk.block_at(static_cast<std::size_t>(rx), ry - 1, static_cast<std::size_t>(rz)) == kStateGrass) {
                    chunk.set_block(static_cast<std::size_t>(rx), ry, static_cast<std::size_t>(rz), flower_state);
                }
            }
        }
        return true;
    }
};

// 1.12.2 原版针叶林云杉/松树生成器（WorldGenTaiga1 逐行转写）
class WorldGenTaiga1 {
public:
    static bool generate(Chunk& chunk, JavaRandom& rand, int x, int y, int z) noexcept {
        const int height = rand.next_int(5) + 7;
        const int canopy_height = height - rand.next_int(2) - 3;
        const int top_y = y + height;
        if (y < 1 || top_y + 1 > 255) return false;

        // 空间校验：检查树干与树冠范围
        for (int cy = y; cy <= top_y + 1; ++cy) {
            int radius = 1;
            if (cy == y) radius = 0;
            if (cy >= y + canopy_height) radius = 2;

            for (int cx = x - radius; cx <= x + radius; ++cx) {
                for (int cz = z - radius; cz <= z + radius; ++cz) {
                    if (cx < 0 || cx >= 16 || cz < 0 || cz >= 16 || cy >= 256) {
                        return false;
                    }
                    const auto state = chunk.block_at(static_cast<std::size_t>(cx), cy, static_cast<std::size_t>(cz));
                    if (state != kStateAir && state != kStateSpruceLeaves) {
                        return false;
                    }
                }
            }
        }

        const auto soil = chunk.block_at(static_cast<std::size_t>(x), y - 1, static_cast<std::size_t>(z));
        if (soil != kStateGrass && soil != kStateDirt && soil != kStatePodzol) return false;
        chunk.set_block(static_cast<std::size_t>(x), y - 1, static_cast<std::size_t>(z), kStateDirt);

        int radius = 0;
        for (int cy = top_y; cy >= y + canopy_height; --cy) {
            for (int cx = x - radius; cx <= x + radius; ++cx) {
                const int dx = cx - x;
                for (int cz = z - radius; cz <= z + radius; ++cz) {
                    const int dz = cz - z;
                    if (std::abs(dx) != radius || std::abs(dz) != radius || radius <= 0) {
                        if (cx >= 0 && cx < 16 && cz >= 0 && cz < 16 && cy >= 0 && cy < 256) {
                            if (chunk.block_at(static_cast<std::size_t>(cx), cy, static_cast<std::size_t>(cz)) == kStateAir) {
                                chunk.set_block(static_cast<std::size_t>(cx), cy, static_cast<std::size_t>(cz), kStateSpruceLeaves);
                            }
                        }
                    }
                }
            }
            if (radius >= 1 && cy == y + canopy_height + 1) {
                --radius;
            } else if (radius < 2) {
                ++radius;
            }
        }

        for (int cy = 0; cy < height - 1; ++cy) {
            if (chunk.block_at(static_cast<std::size_t>(x), y + cy, static_cast<std::size_t>(z)) == kStateAir ||
                chunk.block_at(static_cast<std::size_t>(x), y + cy, static_cast<std::size_t>(z)) == kStateSpruceLeaves) {
                chunk.set_block(static_cast<std::size_t>(x), y + cy, static_cast<std::size_t>(z), kStateSpruceLog);
            }
        }
        return true;
    }
};

// 1.12.2 原版金合欢树生成器（WorldGenSavannaTree 逐行转写）
class WorldGenSavannaTree {
public:
    static bool generate(Chunk& chunk, JavaRandom& rand, int x, int y, int z) noexcept {
        const int height = rand.next_int(3) + 6;
        if (y < 1 || y + height + 1 > 255) return false;

        for (int cy = y; cy <= y + height + 1; ++cy) {
            const int r = cy >= y + height - 1 ? 2 : 1;
            for (int cx = x - r; cx <= x + r; ++cx) {
                for (int cz = z - r; cz <= z + r; ++cz) {
                    if (cx < 0 || cx >= 16 || cz < 0 || cz >= 16 || cy >= 256) return false;
                    const auto s = chunk.block_at(static_cast<std::size_t>(cx), cy, static_cast<std::size_t>(cz));
                    if (s != kStateAir && s != kStateAcaciaLeaves) return false;
                }
            }
        }

        const auto soil = chunk.block_at(static_cast<std::size_t>(x), y - 1, static_cast<std::size_t>(z));
        if (soil != kStateGrass && soil != kStateDirt) return false;
        chunk.set_block(static_cast<std::size_t>(x), y - 1, static_cast<std::size_t>(z), kStateDirt);

        for (int cy = 0; cy < height; ++cy) {
            chunk.set_block(static_cast<std::size_t>(x), y + cy, static_cast<std::size_t>(z), kStateAcaciaLog);
        }

        const int top_y = y + height;
        for (int cy = top_y - 1; cy <= top_y; ++cy) {
            const int r = cy == top_y ? 1 : 2;
            for (int cx = x - r; cx <= x + r; ++cx) {
                for (int cz = z - r; cz <= z + r; ++cz) {
                    if (cx >= 0 && cx < 16 && cz >= 0 && cz < 16 && cy >= 0 && cy < 256) {
                        if (chunk.block_at(static_cast<std::size_t>(cx), cy, static_cast<std::size_t>(cz)) == kStateAir) {
                            chunk.set_block(static_cast<std::size_t>(cx), cy, static_cast<std::size_t>(cz), kStateAcaciaLeaves);
                        }
                    }
                }
            }
        }
        return true;
    }
};

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

        // 边界密闭性校验：湖泊边缘不能侵入水体/熔岩海，底部不能悬空（原版 WorldGenLakes 逐行对齐）
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

// 1.12.2 原版甘蔗生成器（WorldGenReed）
class WorldGenReed {
public:
    static bool generate(Chunk& chunk, JavaRandom& rand, int x, int y, int z) noexcept {
        for (int i = 0; i < 20; ++i) {
            const int rx = x + rand.next_int(4) - rand.next_int(4);
            const int rz = z + rand.next_int(4) - rand.next_int(4);
            if (rx < 1 || rx >= 15 || rz < 1 || rz >= 15 || y < 1 || y >= 250) continue;
            const auto sx = static_cast<std::size_t>(rx);
            const auto sz = static_cast<std::size_t>(rz);
            if (chunk.block_at(sx, y, sz) != kStateAir) continue;

            const auto soil = chunk.block_at(sx, y - 1, sz);
            if (soil != kStateGrass && soil != kStateDirt && soil != kStateSand) continue;

            const bool near_water = chunk.block_at(sx - 1, y - 1, sz) == kStateWater ||
                                    chunk.block_at(sx + 1, y - 1, sz) == kStateWater ||
                                    chunk.block_at(sx, y - 1, sz - 1) == kStateWater ||
                                    chunk.block_at(sx, y - 1, sz + 1) == kStateWater;
            if (!near_water) continue;

            const int height = 2 + rand.next_int(rand.next_int(3) + 1);
            for (int h = 0; h < height; ++h) {
                if (chunk.block_at(sx, y + h, sz) == kStateAir) {
                    chunk.set_block(sx, y + h, sz, kStateSugarCane);
                }
            }
        }
        return true;
    }
};

// 1.12.2 原版仙人掌生成器（WorldGenCactus）
class WorldGenCactus {
public:
    static bool generate(Chunk& chunk, JavaRandom& rand, int x, int y, int z) noexcept {
        for (int i = 0; i < 10; ++i) {
            const int rx = x + rand.next_int(8) - rand.next_int(8);
            const int rz = z + rand.next_int(8) - rand.next_int(8);
            const int ry = y + rand.next_int(4) - rand.next_int(4);
            if (rx < 1 || rx >= 15 || rz < 1 || rz >= 15 || ry < 1 || ry >= 250) continue;
            const auto sx = static_cast<std::size_t>(rx);
            const auto sz = static_cast<std::size_t>(rz);
            if (chunk.block_at(sx, ry, sz) != kStateAir) continue;
            if (chunk.block_at(sx, ry - 1, sz) != kStateSand) continue;

            if (is_solid(chunk.block_at(sx - 1, ry, sz)) || is_solid(chunk.block_at(sx + 1, ry, sz)) ||
                is_solid(chunk.block_at(sx, ry, sz - 1)) || is_solid(chunk.block_at(sx, ry, sz + 1))) {
                continue;
            }

            const int height = 1 + rand.next_int(rand.next_int(3) + 1);
            for (int h = 0; h < height; ++h) {
                chunk.set_block(sx, ry + h, sz, kStateCactus);
            }
        }
        return true;
    }
};

// 1.12.2 原版野生南瓜生成器（WorldGenPumpkin）
class WorldGenPumpkin {
public:
    static bool generate(Chunk& chunk, JavaRandom& rand, int x, int y, int z) noexcept {
        for (int i = 0; i < 64; ++i) {
            const int rx = x + rand.next_int(8) - rand.next_int(8);
            const int rz = z + rand.next_int(8) - rand.next_int(8);
            const int ry = y + rand.next_int(4) - rand.next_int(4);
            if (rx >= 0 && rx < 16 && rz >= 0 && rz < 16 && ry >= 1 && ry < 255) {
                if (chunk.block_at(static_cast<std::size_t>(rx), ry, static_cast<std::size_t>(rz)) == kStateAir &&
                    chunk.block_at(static_cast<std::size_t>(rx), ry - 1, static_cast<std::size_t>(rz)) == kStateGrass) {
                    chunk.set_block(static_cast<std::size_t>(rx), ry, static_cast<std::size_t>(rz), kStatePumpkin);
                }
            }
        }
        return true;
    }
};

// 1.12.2 原版睡莲生成器（WorldGenWaterlily）
class WorldGenWaterlily {
public:
    static bool generate(Chunk& chunk, JavaRandom& rand, int x, int y, int z) noexcept {
        for (int i = 0; i < 10; ++i) {
            const int rx = x + rand.next_int(8) - rand.next_int(8);
            const int rz = z + rand.next_int(8) - rand.next_int(8);
            const int ry = y + rand.next_int(4) - rand.next_int(4);
            if (rx >= 0 && rx < 16 && rz >= 0 && rz < 16 && ry >= 1 && ry < 255) {
                if (chunk.block_at(static_cast<std::size_t>(rx), ry, static_cast<std::size_t>(rz)) == kStateAir &&
                    chunk.block_at(static_cast<std::size_t>(rx), ry - 1, static_cast<std::size_t>(rz)) == kStateWater) {
                    chunk.set_block(static_cast<std::size_t>(rx), ry, static_cast<std::size_t>(rz), kStateWaterlily);
                }
            }
        }
        return true;
    }
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

// 1.12.2 原版地牢生成器（WorldGenDungeons 逐行转写）
class WorldGenDungeons {
public:
    static bool generate(Chunk& chunk, JavaRandom& rand, int x, int y, int z) noexcept {
        const int rx = rand.next_int(2) + 2;
        const int rz = rand.next_int(2) + 2;
        int openings = 0;

        for (int dx = -rx - 1; dx <= rx + 1; ++dx) {
            for (int dy = -1; dy <= 4; ++dy) {
                for (int dz = -rz - 1; dz <= rz + 1; ++dz) {
                    const int bx = x + dx;
                    const int by = y + dy;
                    const int bz = z + dz;
                    if (bx < 0 || bx >= 16 || bz < 0 || bz >= 16 || by < 1 || by >= 255) return false;
                    const auto s = chunk.block_at(static_cast<std::size_t>(bx), by, static_cast<std::size_t>(bz));
                    const bool solid = is_solid(s);
                    if (dy == -1 && !solid) return false;
                    if (dy == 4 && !solid) return false;
                    if ((dx == -rx - 1 || dx == rx + 1 || dz == -rz - 1 || dz == rz + 1) &&
                        dy == 0 && s == kStateAir &&
                        chunk.block_at(static_cast<std::size_t>(bx), by + 1, static_cast<std::size_t>(bz)) == kStateAir) {
                        ++openings;
                    }
                }
            }
        }

        if (openings < 1 || openings > 5) return false;

        for (int dx = -rx - 1; dx <= rx + 1; ++dx) {
            for (int dy = 3; dy >= -1; --dy) {
                for (int dz = -rz - 1; dz <= rz + 1; ++dz) {
                    const int bx = x + dx;
                    const int by = y + dy;
                    const int bz = z + dz;
                    if (dx != -rx - 1 && dy != -1 && dz != -rz - 1 &&
                        dx != rx + 1 && dy != 4 && dz != rz + 1) {
                        chunk.set_block(static_cast<std::size_t>(bx), by, static_cast<std::size_t>(bz), kStateAir);
                    } else if (dy == -1) {
                        chunk.set_block(static_cast<std::size_t>(bx), by, static_cast<std::size_t>(bz),
                                        rand.next_int(4) == 0 ? kStateCobblestone : kStateMossyCobblestone);
                    } else if (is_solid(chunk.block_at(static_cast<std::size_t>(bx), by, static_cast<std::size_t>(bz)))) {
                        chunk.set_block(static_cast<std::size_t>(bx), by, static_cast<std::size_t>(bz), kStateCobblestone);
                    }
                }
            }
        }

        chunk.set_block(static_cast<std::size_t>(x), y, static_cast<std::size_t>(z), kStateMobSpawner);
        for (int i = 0; i < 2; ++i) {
            const int cx = x + rand.next_int(rx * 2 + 1) - rx;
            const int cz = z + rand.next_int(rz * 2 + 1) - rz;
            if (cx >= 0 && cx < 16 && cz >= 0 && cz < 16 &&
                chunk.block_at(static_cast<std::size_t>(cx), y, static_cast<std::size_t>(cz)) == kStateAir) {
                chunk.set_block(static_cast<std::size_t>(cx), y, static_cast<std::size_t>(cz), kStateChest);
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

// 1.12.2 原版枯灌木生成器（WorldGenDeadBush）
class WorldGenDeadBush {
public:
    static bool generate(Chunk& chunk, JavaRandom& rand, int x, int y, int z) noexcept {
        for (int i = 0; i < 4; ++i) {
            const int rx = x + rand.next_int(8) - rand.next_int(8);
            const int rz = z + rand.next_int(8) - rand.next_int(8);
            const int ry = y + rand.next_int(4) - rand.next_int(4);
            if (rx >= 0 && rx < 16 && rz >= 0 && rz < 16 && ry >= 1 && ry < 255) {
                if (chunk.block_at(static_cast<std::size_t>(rx), ry, static_cast<std::size_t>(rz)) == kStateAir &&
                    (chunk.block_at(static_cast<std::size_t>(rx), ry - 1, static_cast<std::size_t>(rz)) == kStateSand ||
                     chunk.block_at(static_cast<std::size_t>(rx), ry - 1, static_cast<std::size_t>(rz)) == kStateDirt)) {
                    chunk.set_block(static_cast<std::size_t>(rx), ry, static_cast<std::size_t>(rz), kStateDeadBush);
                }
            }
        }
        return true;
    }
};

// 1.12.2 原版双层高植物生成器（WorldGenDoublePlant）
class WorldGenDoublePlant {
public:
    static bool generate(Chunk& chunk, JavaRandom& rand, int x, int y, int z, int variant) noexcept {
        for (int i = 0; i < 16; ++i) {
            const int rx = x + rand.next_int(8) - rand.next_int(8);
            const int rz = z + rand.next_int(8) - rand.next_int(8);
            const int ry = y + rand.next_int(4) - rand.next_int(4);
            if (rx >= 0 && rx < 16 && rz >= 0 && rz < 16 && ry >= 1 && ry < 254) {
                if (chunk.block_at(static_cast<std::size_t>(rx), ry, static_cast<std::size_t>(rz)) == kStateAir &&
                    chunk.block_at(static_cast<std::size_t>(rx), ry + 1, static_cast<std::size_t>(rz)) == kStateAir &&
                    chunk.block_at(static_cast<std::size_t>(rx), ry - 1, static_cast<std::size_t>(rz)) == kStateGrass) {
                    chunk.set_block(static_cast<std::size_t>(rx), ry, static_cast<std::size_t>(rz),
                                    static_cast<std::uint16_t>(kStateDoublePlant | (variant & 0x7)));
                    chunk.set_block(static_cast<std::size_t>(rx), ry + 1, static_cast<std::size_t>(rz),
                                    static_cast<std::uint16_t>(kStateDoublePlant | 0x8));
                }
            }
        }
        return true;
    }
};

// 1.12.2 原版地表蘑菇生成器（WorldGenBush）
class WorldGenBush {
public:
    static bool generate(Chunk& chunk, JavaRandom& rand, int x, int y, int z, std::uint16_t mushroom_state) noexcept {
        for (int i = 0; i < 64; ++i) {
            const int rx = x + rand.next_int(8) - rand.next_int(8);
            const int rz = z + rand.next_int(8) - rand.next_int(8);
            const int ry = y + rand.next_int(4) - rand.next_int(4);
            if (rx >= 0 && rx < 16 && rz >= 0 && rz < 16 && ry >= 1 && ry < 255) {
                const auto soil = chunk.block_at(static_cast<std::size_t>(rx), ry - 1, static_cast<std::size_t>(rz));
                if (chunk.block_at(static_cast<std::size_t>(rx), ry, static_cast<std::size_t>(rz)) == kStateAir &&
                    (soil == kStateMycelium || soil == kStatePodzol || soil == kStateDirt || soil == kStateStone)) {
                    chunk.set_block(static_cast<std::size_t>(rx), ry, static_cast<std::size_t>(rz), mushroom_state);
                }
            }
        }
        return true;
    }
};

// 1.12.2 原版丛林西瓜生成器（WorldGenMelon）
class WorldGenMelon {
public:
    static bool generate(Chunk& chunk, JavaRandom& rand, int x, int y, int z) noexcept {
        for (int i = 0; i < 64; ++i) {
            const int rx = x + rand.next_int(8) - rand.next_int(8);
            const int rz = z + rand.next_int(8) - rand.next_int(8);
            const int ry = y + rand.next_int(4) - rand.next_int(4);
            if (rx >= 0 && rx < 16 && rz >= 0 && rz < 16 && ry >= 1 && ry < 255) {
                if (chunk.block_at(static_cast<std::size_t>(rx), ry, static_cast<std::size_t>(rz)) == kStateAir &&
                    chunk.block_at(static_cast<std::size_t>(rx), ry - 1, static_cast<std::size_t>(rz)) == kStateGrass) {
                    chunk.set_block(static_cast<std::size_t>(rx), ry, static_cast<std::size_t>(rz), kStateMelon);
                }
            }
        }
        return true;
    }
};

// 1.12.2 原版沼泽树生成器（WorldGenSwamp）
class WorldGenSwamp {
public:
    static bool generate(Chunk& chunk, JavaRandom& rand, int x, int y, int z) noexcept {
        const int height = rand.next_int(4) + 5;
        if (y < 1 || y + height + 1 > 255) return false;

        const auto soil = chunk.block_at(static_cast<std::size_t>(x), y - 1, static_cast<std::size_t>(z));
        if (soil != kStateGrass && soil != kStateDirt && soil != kStateWater) return false;
        chunk.set_block(static_cast<std::size_t>(x), y - 1, static_cast<std::size_t>(z), kStateDirt);

        for (int cy = y - 3 + height; cy <= y + height; ++cy) {
            const int offset = cy - (y + height);
            const int r = 2 - offset / 2;
            for (int cx = x - r; cx <= x + r; ++cx) {
                for (int cz = z - r; cz <= z + r; ++cz) {
                    if (cx >= 0 && cx < 16 && cz >= 0 && cz < 16 && cy >= 0 && cy < 256) {
                        if (chunk.block_at(static_cast<std::size_t>(cx), cy, static_cast<std::size_t>(cz)) == kStateAir) {
                            chunk.set_block(static_cast<std::size_t>(cx), cy, static_cast<std::size_t>(cz), kStateOakLeaves);
                        }
                    }
                }
            }
        }

        for (int cy = 0; cy < height; ++cy) {
            chunk.set_block(static_cast<std::size_t>(x), y + cy, static_cast<std::size_t>(z), kStateOakLog);
        }
        return true;
    }
};

// 1.12.2 原版深色橡树生成器（WorldGenCanopyTree）
class WorldGenCanopyTree {
public:
    static bool generate(Chunk& chunk, JavaRandom& rand, int x, int y, int z) noexcept {
        const int height = rand.next_int(3) + 6;
        if (x < 2 || x >= 14 || z < 2 || z >= 14 || y < 1 || y + height + 2 > 255) return false;

        for (int cy = 0; cy < height; ++cy) {
            for (int dx = 0; dx <= 1; ++dx) {
                for (int dz = 0; dz <= 1; ++dz) {
                    chunk.set_block(static_cast<std::size_t>(x + dx), y + cy, static_cast<std::size_t>(z + dz), kStateDarkOakLog);
                }
            }
        }

        const int top_y = y + height;
        for (int cy = top_y - 2; cy <= top_y + 1; ++cy) {
            const int r = cy >= top_y ? 2 : 3;
            for (int cx = x - r; cx <= x + 1 + r; ++cx) {
                for (int cz = z - r; cz <= z + 1 + r; ++cz) {
                    if (cx >= 0 && cx < 16 && cz >= 0 && cz < 16 && cy >= 0 && cy < 256) {
                        if (chunk.block_at(static_cast<std::size_t>(cx), cy, static_cast<std::size_t>(cz)) == kStateAir) {
                            chunk.set_block(static_cast<std::size_t>(cx), cy, static_cast<std::size_t>(cz), kStateDarkOakLeaves);
                        }
                    }
                }
            }
        }
        return true;
    }
};

// 1.12.2 原版沙漠水井生成器（WorldGenDesertWells）
class WorldGenDesertWells {
public:
    static bool generate(Chunk& chunk, int x, int y, int z) noexcept {
        if (x < 2 || x >= 14 || z < 2 || z >= 14 || y < 2 || y >= 250) return false;
        for (int dx = -2; dx <= 2; ++dx) {
            for (int dz = -2; dz <= 2; ++dz) {
                const auto sx = static_cast<std::size_t>(x + dx);
                const auto sz = static_cast<std::size_t>(z + dz);
                chunk.set_block(sx, y - 1, sz, kStateSandstone);
                if (dx == 0 && dz == 0) {
                    chunk.set_block(sx, y, sz, kStateWater);
                } else if (std::abs(dx) == 2 || std::abs(dz) == 2) {
                    chunk.set_block(sx, y, sz, kStateSandstone);
                }
            }
        }
        return true;
    }
};

// 1.12.2 原版针叶林苔石巨石生成器（WorldGenBlockBlob）
class WorldGenBlockBlob {
public:
    static bool generate(Chunk& chunk, JavaRandom& rand, int x, int y, int z) noexcept {
        const int r = rand.next_int(2) + 2;
        for (int cx = x - r; cx <= x + r; ++cx) {
            const int dx = cx - x;
            for (int cz = z - r; cz <= z + r; ++cz) {
                const int dz = cz - z;
                for (int cy = y - r; cy <= y + r; ++cy) {
                    const int dy = cy - y;
                    if (dx * dx + dy * dy + dz * dz <= r * r) {
                        if (cx >= 0 && cx < 16 && cz >= 0 && cz < 16 && cy >= 1 && cy < 255) {
                            chunk.set_block(static_cast<std::size_t>(cx), cy, static_cast<std::size_t>(cz), kStateMossyCobblestone);
                        }
                    }
                }
            }
        }
        return true;
    }
};

}  // namespace cyane::world::gen
