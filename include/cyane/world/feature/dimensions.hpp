#pragma once

#include <cmath>
#include <cstdlib>
#include "cyane/world/blocks.hpp"
#include "cyane/world/chunk.hpp"
#include "cyane/world/noise.hpp"

namespace cyane::world::gen {

// 1.12.2 原版下界荧石簇生成器（WorldGenGlowStone1 / WorldGenGlowStone2）
class WorldGenGlowStone1 {
public:
    static bool generate(Chunk& chunk, JavaRandom& rand, int x, int y, int z) noexcept {
        if (x < 0 || x >= 16 || z < 0 || z >= 16 || y < 1 || y >= 127) return false;
        const auto sx = static_cast<std::size_t>(x);
        const auto sz = static_cast<std::size_t>(z);
        if (chunk.block_at(sx, y, sz) != kStateAir) return false;
        if (chunk.block_at(sx, y + 1, sz) != kStateNetherrack) return false;

        chunk.set_block(sx, y, sz, kStateGlowstone);

        for (int i = 0; i < 1500; ++i) {
            const int rx = x + rand.next_int(8) - rand.next_int(8);
            const int rz = z + rand.next_int(8) - rand.next_int(8);
            const int ry = y - rand.next_int(12);
            if (rx >= 0 && rx < 16 && rz >= 0 && rz < 16 && ry >= 1 && ry < 127) {
                const auto rsx = static_cast<std::size_t>(rx);
                const auto rsz = static_cast<std::size_t>(rz);
                if (chunk.block_at(rsx, ry, rsz) == kStateAir) {
                    int neighbor_glowstone = 0;
                    if (rx > 0 && chunk.block_at(rsx - 1, ry, rsz) == kStateGlowstone) ++neighbor_glowstone;
                    if (rx < 15 && chunk.block_at(rsx + 1, ry, rsz) == kStateGlowstone) ++neighbor_glowstone;
                    if (rz > 0 && chunk.block_at(rsx, ry, rsz - 1) == kStateGlowstone) ++neighbor_glowstone;
                    if (rz < 15 && chunk.block_at(rsx, ry, rsz + 1) == kStateGlowstone) ++neighbor_glowstone;
                    if (ry < 127 && chunk.block_at(rsx, ry + 1, rsz) == kStateGlowstone) ++neighbor_glowstone;

                    if (neighbor_glowstone == 1) {
                        chunk.set_block(rsx, ry, rsz, kStateGlowstone);
                    }
                }
            }
        }
        return true;
    }
};

// 1.12.2 原版下界火生成器（WorldGenFire）
class WorldGenFire {
public:
    static bool generate(Chunk& chunk, JavaRandom& rand, int x, int y, int z) noexcept {
        for (int i = 0; i < 64; ++i) {
            const int rx = x + rand.next_int(8) - rand.next_int(8);
            const int rz = z + rand.next_int(8) - rand.next_int(8);
            const int ry = y + rand.next_int(4) - rand.next_int(4);
            if (rx >= 0 && rx < 16 && rz >= 0 && rz < 16 && ry >= 1 && ry < 127) {
                const auto sx = static_cast<std::size_t>(rx);
                const auto sz = static_cast<std::size_t>(rz);
                if (chunk.block_at(sx, ry, sz) == kStateAir &&
                    chunk.block_at(sx, ry - 1, sz) == kStateNetherrack) {
                    chunk.set_block(sx, ry, sz, kStateFire);
                }
            }
        }
        return true;
    }
};

// 1.12.2 原版下界岩浆泉生成器（WorldGenHellLava）
class WorldGenHellLava {
public:
    static bool generate(Chunk& chunk, int x, int y, int z) noexcept {
        if (x < 1 || x >= 15 || z < 1 || z >= 15 || y < 1 || y >= 127) return false;
        const auto sx = static_cast<std::size_t>(x);
        const auto sz = static_cast<std::size_t>(z);

        if (chunk.block_at(sx, y + 1, sz) != kStateNetherrack) return false;
        if (chunk.block_at(sx, y - 1, sz) != kStateNetherrack) return false;
        if (chunk.block_at(sx, y, sz) != kStateAir && chunk.block_at(sx, y, sz) != kStateNetherrack) return false;

        int netherrack_count = 0;
        int air_count = 0;
        if (chunk.block_at(sx - 1, y, sz) == kStateNetherrack) ++netherrack_count;
        else if (chunk.block_at(sx - 1, y, sz) == kStateAir) ++air_count;

        if (chunk.block_at(sx + 1, y, sz) == kStateNetherrack) ++netherrack_count;
        else if (chunk.block_at(sx + 1, y, sz) == kStateAir) ++air_count;

        if (chunk.block_at(sx, y, sz - 1) == kStateNetherrack) ++netherrack_count;
        else if (chunk.block_at(sx, y, sz - 1) == kStateAir) ++air_count;

        if (chunk.block_at(sx, y, sz + 1) == kStateNetherrack) ++netherrack_count;
        else if (chunk.block_at(sx, y, sz + 1) == kStateAir) ++air_count;

        if (netherrack_count == 4 && air_count == 1) {
            chunk.set_block(sx, y, sz, kStateLava);
            return true;
        }
        return false;
    }
};

// 1.12.2 原版末地黑曜石柱生成器（WorldGenSpikes）
class WorldGenSpikes {
public:
    static bool generate(Chunk& chunk, int x, int y, int z, int radius, int height, bool guarded = false) noexcept {
        if (y < 1 || y + height + 1 > 255) return false;
        for (int cx = x - radius; cx <= x + radius; ++cx) {
            const int dx = cx - x;
            for (int cz = z - radius; cz <= z + radius; ++cz) {
                const int dz = cz - z;
                if (dx * dx + dz * dz <= radius * radius + 1) {
                    if (cx >= 0 && cx < 16 && cz >= 0 && cz < 16) {
                        for (int cy = y; cy < y + height; ++cy) {
                            chunk.set_block(static_cast<std::size_t>(cx), cy, static_cast<std::size_t>(cz), kStateObsidian);
                        }
                    }
                }
            }
        }

        if (guarded && x >= 2 && x < 14 && z >= 2 && z < 14 && y + height + 2 < 255) {
            for (int dx = -2; dx <= 2; ++dx) {
                for (int dz = -2; dz <= 2; ++dz) {
                    const auto sx = static_cast<std::size_t>(x + dx);
                    const auto sz = static_cast<std::size_t>(z + dz);
                    if (std::abs(dx) == 2 || std::abs(dz) == 2) {
                        for (int cy = y + height; cy <= y + height + 3; ++cy) {
                            chunk.set_block(sx, cy, sz, kStateIronBars);
                        }
                    }
                }
            }
        }
        return true;
    }
};

// 1.12.2 原版末地祭坛传送门基座生成器（WorldGenEndPodium）
class WorldGenEndPodium {
public:
    static bool generate(Chunk& chunk, int x, int y, int z) noexcept {
        if (y < 4 || y >= 250) return false;
        for (int dx = -4; dx <= 4; ++dx) {
            const int bx = x + dx;
            if (bx < 0 || bx >= 16) continue;
            for (int dz = -4; dz <= 4; ++dz) {
                const int bz = z + dz;
                if (bz < 0 || bz >= 16) continue;
                const int dist_sq = dx * dx + dz * dz;
                if (dist_sq <= 16) {
                    const auto sx = static_cast<std::size_t>(bx);
                    const auto sz = static_cast<std::size_t>(bz);
                    chunk.set_block(sx, y - 1, sz, kStateBedrock);
                    if (dist_sq <= 4) {
                        chunk.set_block(sx, y, sz, kStateEndPortal);
                    } else {
                        chunk.set_block(sx, y, sz, kStateBedrock);
                    }
                }
            }
        }
        if (x >= 0 && x < 16 && z >= 0 && z < 16) {
            const auto sx = static_cast<std::size_t>(x);
            const auto sz = static_cast<std::size_t>(z);
            for (int cy = y; cy <= y + 3; ++cy) {
                chunk.set_block(sx, cy, sz, kStateBedrock);
            }
            chunk.set_block(sx, y + 4, sz, kStateTorch);
        }
        return true;
    }
};

// 1.12.2 原版末地折跃门生成器（WorldGenEndGateway）
class WorldGenEndGateway {
public:
    static bool generate(Chunk& chunk, int x, int y, int z) noexcept {
        if (x < 1 || x >= 15 || z < 1 || z >= 15 || y < 2 || y >= 253) return false;
        for (int dx = -1; dx <= 1; ++dx) {
            for (int dy = -2; dy <= 2; ++dy) {
                for (int dz = -1; dz <= 1; ++dz) {
                    const auto sx = static_cast<std::size_t>(x + dx);
                    const auto sz = static_cast<std::size_t>(z + dz);
                    const int d_sum = std::abs(dx) + std::abs(dy) + std::abs(dz);
                    if (d_sum == 0) {
                        chunk.set_block(sx, y + dy, sz, kStateEndGateway);
                    } else if (d_sum <= 2) {
                        chunk.set_block(sx, y + dy, sz, kStateBedrock);
                    }
                }
            }
        }
        return true;
    }
};

// 1.12.2 原版末地浮岛生成器（WorldGenEndIsland）
class WorldGenEndIsland {
public:
    static bool generate(Chunk& chunk, JavaRandom& rand, int x, int y, int z) noexcept {
        float radius = static_cast<float>(rand.next_int(3) + 4);
        for (int dy = 0; radius > 0.5f; --dy) {
            for (int dx = -static_cast<int>(std::floor(radius)); dx <= static_cast<int>(std::ceil(radius)); ++dx) {
                for (int dz = -static_cast<int>(std::floor(radius)); dz <= static_cast<int>(std::ceil(radius)); ++dz) {
                    if (static_cast<float>(dx * dx + dz * dz) <= (radius + 1.0f) * (radius + 1.0f)) {
                        const int bx = x + dx;
                        const int by = y + dy;
                        const int bz = z + dz;
                        if (bx >= 0 && bx < 16 && bz >= 0 && bz < 16 && by >= 1 && by < 255) {
                            chunk.set_block(static_cast<std::size_t>(bx), by, static_cast<std::size_t>(bz), kStateEndStone);
                        }
                    }
                }
            }
            radius -= static_cast<float>(rand.next_int(2)) + 0.5f;
        }
        return true;
    }
};

}  // namespace cyane::world::gen
