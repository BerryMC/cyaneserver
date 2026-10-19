#pragma once

#include <cstdlib>
#include "cyane/world/blocks.hpp"
#include "cyane/world/chunk.hpp"
#include "cyane/world/noise.hpp"

namespace cyane::world::gen {

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

}  // namespace cyane::world::gen
