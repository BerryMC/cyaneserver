#pragma once

#include <cmath>
#include <cstdlib>
#include "cyane/world/blocks.hpp"
#include "cyane/world/chunk.hpp"
#include "cyane/world/noise.hpp"

namespace cyane::world::gen {

// 1.12.2 原版地表杂草生成器（WorldGenTallGrass）
class WorldGenTallGrass {
public:
    static bool generate(Chunk& chunk, JavaRandom& rand, int x, int y, int z) noexcept {
        for (int i = 0; i < 128; ++i) {
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

// 1.12.2 原版丛林矮灌木生成器（WorldGenShrub）
class WorldGenShrub {
public:
    static bool generate(Chunk& chunk, JavaRandom& rand, int x, int y, int z) noexcept {
        if (x < 1 || x >= 15 || z < 1 || z >= 15 || y < 1 || y >= 253) return false;
        const auto soil = chunk.block_at(static_cast<std::size_t>(x), y - 1, static_cast<std::size_t>(z));
        if (soil != kStateGrass && soil != kStateDirt) return false;
        chunk.set_block(static_cast<std::size_t>(x), y - 1, static_cast<std::size_t>(z), kStateDirt);

        const int height = rand.next_int(2) + 1;
        for (int cy = 0; cy < height; ++cy) {
            chunk.set_block(static_cast<std::size_t>(x), y + cy, static_cast<std::size_t>(z), kStateOakLog);
        }

        for (int cy = y; cy <= y + height; ++cy) {
            const int r = 1;
            for (int cx = x - r; cx <= x + r; ++cx) {
                for (int cz = z - r; cz <= z + r; ++cz) {
                    if (std::abs(cx - x) != r || std::abs(cz - z) != r || rand.next_int(2) == 0) {
                        if (chunk.block_at(static_cast<std::size_t>(cx), cy, static_cast<std::size_t>(cz)) == kStateAir) {
                            chunk.set_block(static_cast<std::size_t>(cx), cy, static_cast<std::size_t>(cz), kStateOakLeaves);
                        }
                    }
                }
            }
        }
        return true;
    }
};

// 1.12.2 原版巨型蘑菇生成器（WorldGenBigMushroom：棕色与红色巨菇）
class WorldGenBigMushroom {
public:
    static bool generate(Chunk& chunk, JavaRandom& rand, int x, int y, int z, bool is_red) noexcept {
        const int height = rand.next_int(3) + 4;
        if (y < 1 || y + height + 1 > 255 || x < 3 || x >= 13 || z < 3 || z >= 13) return false;

        const auto soil = chunk.block_at(static_cast<std::size_t>(x), y - 1, static_cast<std::size_t>(z));
        if (soil != kStateGrass && soil != kStateDirt && soil != kStateMycelium) return false;

        const auto cap_state = is_red ? kStateHugeRedMushroom : kStateHugeBrownMushroom;

        // 菌柄
        for (int cy = 0; cy < height; ++cy) {
            chunk.set_block(static_cast<std::size_t>(x), y + cy, static_cast<std::size_t>(z),
                            static_cast<std::uint16_t>(cap_state | 10)); // 10 = stem
        }

        // 菌盖
        if (is_red) {
            for (int cy = y + height - 3; cy <= y + height; ++cy) {
                const int r = (cy < y + height) ? 2 : 1;
                for (int cx = x - r; cx <= x + r; ++cx) {
                    for (int cz = z - r; cz <= z + r; ++cz) {
                        const bool corner = (cx == x - r || cx == x + r) && (cz == z - r || cz == z + r);
                        if (!corner || cy < y + height) {
                            if (chunk.block_at(static_cast<std::size_t>(cx), cy, static_cast<std::size_t>(cz)) == kStateAir) {
                                chunk.set_block(static_cast<std::size_t>(cx), cy, static_cast<std::size_t>(cz), cap_state);
                            }
                        }
                    }
                }
            }
        } else {
            const int cy = y + height;
            for (int cx = x - 3; cx <= x + 3; ++cx) {
                for (int cz = z - 3; cz <= z + 3; ++cz) {
                    const bool corner = (std::abs(cx - x) == 3 && std::abs(cz - z) == 3);
                    if (!corner) {
                        if (chunk.block_at(static_cast<std::size_t>(cx), cy, static_cast<std::size_t>(cz)) == kStateAir) {
                            chunk.set_block(static_cast<std::size_t>(cx), cy, static_cast<std::size_t>(cz), cap_state);
                        }
                    }
                }
            }
        }
        return true;
    }
};

// 1.12.2 原版墙面藤蔓生成器（WorldGenVines）
class WorldGenVines {
public:
    static bool generate(Chunk& chunk, JavaRandom& rand, int x, int y, int z) noexcept {
        for (int i = 0; i < 64; ++i) {
            const int rx = x + rand.next_int(4) - rand.next_int(4);
            const int rz = z + rand.next_int(4) - rand.next_int(4);
            const int ry = y + rand.next_int(4) - rand.next_int(4);
            if (rx < 0 || rx >= 16 || rz < 0 || rz >= 16 || ry < 1 || ry >= 255) continue;
            const auto sx = static_cast<std::size_t>(rx);
            const auto sz = static_cast<std::size_t>(rz);
            if (chunk.block_at(sx, ry, sz) != kStateAir) continue;

            std::uint16_t meta = 0;
            if (rx > 0 && is_solid(chunk.block_at(sx - 1, ry, sz))) meta |= 0x2; // West
            if (rx < 15 && is_solid(chunk.block_at(sx + 1, ry, sz))) meta |= 0x8; // East
            if (rz > 0 && is_solid(chunk.block_at(sx, ry, sz - 1))) meta |= 0x4; // North
            if (rz < 15 && is_solid(chunk.block_at(sx, ry, sz + 1))) meta |= 0x1; // South
            if (meta != 0) {
                chunk.set_block(sx, ry, sz, static_cast<std::uint16_t>(kStateVine | meta));
            }
        }
        return true;
    }
};

}  // namespace cyane::world::gen
