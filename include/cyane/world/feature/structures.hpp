#pragma once

#include <algorithm>
#include <cstdlib>
#include "cyane/world/blocks.hpp"
#include "cyane/world/chunk.hpp"
#include "cyane/world/noise.hpp"

namespace cyane::world::gen {

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

// 1.12.2 原版地下化石遗骸生成器（WorldGenFossils 逐行转写）
class WorldGenFossils {
public:
    static bool generate(Chunk& chunk, JavaRandom& rand, int x, int y, int z) noexcept {
        if (x < 4 || x >= 12 || z < 4 || z >= 12 || y < 10 || y >= 60) return false;
        const int length = rand.next_int(3) + 4;
        for (int dz = -length; dz <= length; ++dz) {
            const int bz = z + dz;
            if (bz < 0 || bz >= 16) continue;
            chunk.set_block(static_cast<std::size_t>(x), y, static_cast<std::size_t>(bz), kStateBoneBlock);

            if (std::abs(dz) % 2 == 0) {
                for (int dx = 1; dx <= 2; ++dx) {
                    chunk.set_block(static_cast<std::size_t>(x - dx), y - dx + 1, static_cast<std::size_t>(bz), kStateBoneBlock);
                    chunk.set_block(static_cast<std::size_t>(x + dx), y - dx + 1, static_cast<std::size_t>(bz), kStateBoneBlock);
                }
            }

            if (rand.next_int(10) == 0) {
                chunk.set_block(static_cast<std::size_t>(x), y, static_cast<std::size_t>(bz), kStateCoalOre);
            }
        }
        return true;
    }
};

// 1.12.2 原版开局奖励箱生成器（WorldGeneratorBonusChest）
class WorldGeneratorBonusChest {
public:
    static bool generate(Chunk& chunk, JavaRandom& rand, int x, int y, int z) noexcept {
        for (int i = 0; i < 10; ++i) {
            const int rx = x + rand.next_int(4) - rand.next_int(4);
            const int rz = z + rand.next_int(4) - rand.next_int(4);
            const int ry = y + rand.next_int(2) - rand.next_int(2);
            if (rx < 1 || rx >= 15 || rz < 1 || rz >= 15 || ry < 1 || ry >= 254) continue;
            const auto sx = static_cast<std::size_t>(rx);
            const auto sz = static_cast<std::size_t>(rz);
            if (chunk.block_at(sx, ry, sz) == kStateAir &&
                is_solid(chunk.block_at(sx, ry - 1, sz))) {
                chunk.set_block(sx, ry, sz, kStateChest);

                if (chunk.block_at(sx - 1, ry, sz) == kStateAir && is_solid(chunk.block_at(sx - 1, ry - 1, sz))) {
                    chunk.set_block(sx - 1, ry, sz, kStateTorch);
                }
                if (chunk.block_at(sx + 1, ry, sz) == kStateAir && is_solid(chunk.block_at(sx + 1, ry - 1, sz))) {
                    chunk.set_block(sx + 1, ry, sz, kStateTorch);
                }
                if (chunk.block_at(sx, ry, sz - 1) == kStateAir && is_solid(chunk.block_at(sx, ry - 1, sz - 1))) {
                    chunk.set_block(sx, ry, sz - 1, kStateTorch);
                }
                if (chunk.block_at(sx, ry, sz + 1) == kStateAir && is_solid(chunk.block_at(sx, ry - 1, sz + 1))) {
                    chunk.set_block(sx, ry, sz + 1, kStateTorch);
                }
                return true;
            }
        }
        return false;
    }
};

// 1.12.2 原版浮冰刺生成器（WorldGenIceSpike）
class WorldGenIceSpike {
public:
    static bool generate(Chunk& chunk, JavaRandom& rand, int x, int y, int z) noexcept {
        if (y < 50 || y >= 200 || x < 4 || x >= 12 || z < 4 || z >= 12) return false;
        const int height = rand.next_int(16) + 10;
        const int base_r = rand.next_int(2) + 2;

        for (int dx = -base_r; dx <= base_r; ++dx) {
            for (int dz = -base_r; dz <= base_r; ++dz) {
                if (dx * dx + dz * dz <= base_r * base_r) {
                    for (int by = y - 1; by <= y + 1; ++by) {
                        chunk.set_block(static_cast<std::size_t>(x + dx), by, static_cast<std::size_t>(z + dz), kStatePackedIce);
                    }
                }
            }
        }

        for (int h = 0; h < height; ++h) {
            const int r = std::max(1, base_r - h * base_r / height);
            for (int dx = -r; dx <= r; ++dx) {
                for (int dz = -r; dz <= r; ++dz) {
                    if (dx * dx + dz * dz <= r * r) {
                        chunk.set_block(static_cast<std::size_t>(x + dx), y + h, static_cast<std::size_t>(z + dz), kStatePackedIce);
                    }
                }
            }
        }
        return true;
    }
};

// 1.12.2 原版浮冰带/冰道生成器（WorldGenIcePath）
class WorldGenIcePath {
public:
    explicit constexpr WorldGenIcePath(int base_radius = 2) noexcept : radius_(base_radius) {}

    bool generate(Chunk& chunk, JavaRandom& rand, int x, int y, int z) const noexcept {
        const int r = rand.next_int(radius_) + radius_;
        for (int cx = x - r; cx <= x + r; ++cx) {
            const int dx = cx - x;
            for (int cz = z - r; cz <= z + r; ++cz) {
                const int dz = cz - z;
                if (dx * dx + dz * dz <= r * r) {
                    if (cx >= 0 && cx < 16 && cz >= 0 && cz < 16) {
                        for (int cy = y - 1; cy <= y + 1; ++cy) {
                            if (cy >= 1 && cy < 255) {
                                const auto s = chunk.block_at(static_cast<std::size_t>(cx), cy, static_cast<std::size_t>(cz));
                                if (s == kStateDirt || s == kStateGrass || s == kStateSnow) {
                                    chunk.set_block(static_cast<std::size_t>(cx), cy, static_cast<std::size_t>(cz), kStatePackedIce);
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
    int radius_;
};

}  // namespace cyane::world::gen
