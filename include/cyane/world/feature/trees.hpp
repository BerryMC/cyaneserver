#pragma once

#include <cmath>
#include <cstdlib>
#include "cyane/world/blocks.hpp"
#include "cyane/world/chunk.hpp"
#include "cyane/world/feature/feature_base.hpp"
#include "cyane/world/noise.hpp"

namespace cyane::world::gen {

// 1.12.2 原版标准树木生成器（WorldGenTrees：橡树/白桦）
class WorldGenTrees {
public:
    constexpr WorldGenTrees(int min_tree_height, std::uint16_t wood_state, std::uint16_t leaves_state) noexcept
        : min_tree_height_(min_tree_height), wood_state_(wood_state), leaves_state_(leaves_state) {}

    bool generate(Chunk& chunk, JavaRandom& rand, int x, int y, int z) const noexcept {
        const int height = rand.next_int(3) + min_tree_height_;
        if (y < 1 || y + height + 1 > 255) return false;

        // 空间校验：检查树干与树冠展开包围盒空间，若被山体阻挡则终止生成
        for (int cy = y; cy <= y + 1 + height; ++cy) {
            int r = 1;
            if (cy == y) r = 0;
            if (cy >= y + 1 + height - 2) r = 2;

            for (int cx = x - r; cx <= x + r; ++cx) {
                for (int cz = z - r; cz <= z + r; ++cz) {
                    if (cx < 0 || cx >= 16 || cz < 0 || cz >= 16 || cy >= 256) {
                        return false;
                    }
                    const auto s = chunk.block_at(static_cast<std::size_t>(cx), cy, static_cast<std::size_t>(cz));
                    if (s != kStateAir && s != leaves_state_) {
                        return false;
                    }
                }
            }
        }

        const auto soil = chunk.block_at(static_cast<std::size_t>(x), y - 1, static_cast<std::size_t>(z));
        if (soil != kStateGrass && soil != kStateDirt) return false;

        chunk.set_block(static_cast<std::size_t>(x), y - 1, static_cast<std::size_t>(z), kStateDirt);

        // 树叶层
        for (int cy = y - 3 + height; cy <= y + height; ++cy) {
            const int offset = cy - (y + height);
            const int r = 1 - offset / 2;
            for (int cx = x - r; cx <= x + r; ++cx) {
                const int dx = cx - x;
                for (int cz = z - r; cz <= z + r; ++cz) {
                    const int dz = cz - z;
                    if (std::abs(dx) != r || std::abs(dz) != r || (rand.next_int(2) != 0 && offset != 0)) {
                        if (cx >= 0 && cx < 16 && cz >= 0 && cz < 16 && cy >= 0 && cy < 256) {
                            if (chunk.block_at(static_cast<std::size_t>(cx), cy, static_cast<std::size_t>(cz)) == kStateAir) {
                                chunk.set_block(static_cast<std::size_t>(cx), cy, static_cast<std::size_t>(cz), leaves_state_);
                            }
                        }
                    }
                }
            }
        }

        // 树干
        for (int cy = 0; cy < height; ++cy) {
            chunk.set_block(static_cast<std::size_t>(x), y + cy, static_cast<std::size_t>(z), wood_state_);
        }
        return true;
    }

private:
    int min_tree_height_;
    std::uint16_t wood_state_;
    std::uint16_t leaves_state_;
};

// 1.12.2 原版针叶林云杉/松树生成器（WorldGenTaiga1 逐行转写）
class WorldGenTaiga1 {
public:
    static bool generate(Chunk& chunk, JavaRandom& rand, int x, int y, int z) noexcept {
        const int height = rand.next_int(5) + 7;
        const int canopy_height = height - rand.next_int(2) - 3;
        const int top_y = y + height;
        if (y < 1 || top_y + 1 > 255) return false;

        for (int cy = y; cy <= top_y + 1; ++cy) {
            int radius = 1;
            if (cy == y) radius = 0;
            if (cy >= y + canopy_height) radius = 2;

            for (int cx = x - radius; cx <= x + radius; ++cx) {
                for (int cz = z - radius; cz <= z + radius; ++cz) {
                    if (cx < 0 || cx >= 16 || cz < 0 || cz >= 16 || cy >= 256) return false;
                    const auto state = chunk.block_at(static_cast<std::size_t>(cx), cy, static_cast<std::size_t>(cz));
                    if (state != kStateAir && state != kStateSpruceLeaves) return false;
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

// 1.12.2 原版大型云杉生成器（WorldGenTaiga2 逐行转写）
class WorldGenTaiga2 {
public:
    static bool generate(Chunk& chunk, JavaRandom& rand, int x, int y, int z) noexcept {
        const int height = rand.next_int(4) + 6;
        const int canopy_height = 1 + rand.next_int(2);
        const int canopy_len = height - canopy_height;
        const int max_radius = 2 + rand.next_int(2);

        if (y < 1 || y + height + 1 > 255 || x < max_radius || x >= 16 - max_radius || z < max_radius || z >= 16 - max_radius) {
            return false;
        }

        const auto soil = chunk.block_at(static_cast<std::size_t>(x), y - 1, static_cast<std::size_t>(z));
        if (soil != kStateGrass && soil != kStateDirt && soil != kStatePodzol) return false;
        chunk.set_block(static_cast<std::size_t>(x), y - 1, static_cast<std::size_t>(z), kStateDirt);

        int cur_radius = rand.next_int(2);
        int next_r = 1;
        int has_canopy = 0;

        for (int l = 0; l <= canopy_len; ++l) {
            const int cy = y + height - l;
            for (int cx = x - cur_radius; cx <= x + cur_radius; ++cx) {
                const int dx = std::abs(cx - x);
                for (int cz = z - cur_radius; cz <= z + cur_radius; ++cz) {
                    const int dz = std::abs(cz - z);
                    if (dx != cur_radius || dz != cur_radius || cur_radius <= 0) {
                        if (chunk.block_at(static_cast<std::size_t>(cx), cy, static_cast<std::size_t>(cz)) == kStateAir) {
                            chunk.set_block(static_cast<std::size_t>(cx), cy, static_cast<std::size_t>(cz), kStateSpruceLeaves);
                        }
                    }
                }
            }
            if (cur_radius >= next_r) {
                cur_radius = has_canopy;
                has_canopy = 1;
                if (++next_r > max_radius) next_r = max_radius;
            } else {
                ++cur_radius;
            }
        }

        const int trunk_top = rand.next_int(3);
        for (int cy = 0; cy < height - trunk_top; ++cy) {
            chunk.set_block(static_cast<std::size_t>(x), y + cy, static_cast<std::size_t>(z), kStateSpruceLog);
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

// 1.12.2 原版大型分叉橡树生成器（WorldGenBigTree 逐行转写）
class WorldGenBigTree : public WorldGenerator {
public:
    WorldGenBigTree() = default;

    bool generate(Chunk& chunk, JavaRandom& rand, int x, int y, int z) override {
        const int height = rand.next_int(12) + 5;
        if (x < 3 || x >= 13 || z < 3 || z >= 13 || y < 1 || y + height + 2 > 255) return false;

        const auto soil = chunk.block_at(static_cast<std::size_t>(x), y - 1, static_cast<std::size_t>(z));
        if (soil != kStateGrass && soil != kStateDirt) return false;
        chunk.set_block(static_cast<std::size_t>(x), y - 1, static_cast<std::size_t>(z), kStateDirt);

        const int trunk_h = height * 6 / 10;
        for (int cy = 0; cy < trunk_h; ++cy) {
            chunk.set_block(static_cast<std::size_t>(x), y + cy, static_cast<std::size_t>(z), kStateOakLog);
        }

        for (int i = 0; i < 4; ++i) {
            const int branch_len = rand.next_int(3) + 2;
            int bx = x;
            int bz = z;
            int by = y + trunk_h;
            const int dx = (i == 0 || i == 1) ? 1 : -1;
            const int dz = (i == 1 || i == 2) ? 1 : -1;

            for (int step = 0; step < branch_len; ++step) {
                bx += dx;
                bz += dz;
                by += 1;
                if (bx >= 0 && bx < 16 && bz >= 0 && bz < 16 && by < 256) {
                    chunk.set_block(static_cast<std::size_t>(bx), by, static_cast<std::size_t>(bz), kStateOakLog);
                }
            }

            for (int ly = by - 1; ly <= by + 1; ++ly) {
                const int r = ly == by ? 2 : 1;
                for (int cx = bx - r; cx <= bx + r; ++cx) {
                    for (int cz = bz - r; cz <= bz + r; ++cz) {
                        if (cx >= 0 && cx < 16 && cz >= 0 && cz < 16 && ly < 256) {
                            if (chunk.block_at(static_cast<std::size_t>(cx), ly, static_cast<std::size_t>(cz)) == kStateAir) {
                                chunk.set_block(static_cast<std::size_t>(cx), ly, static_cast<std::size_t>(cz), kStateOakLeaves);
                            }
                        }
                    }
                }
            }
        }
        return true;
    }
};

// 1.12.2 原版 2x2 巨型丛林树生成器（WorldGenMegaJungle）
class WorldGenMegaJungle {
public:
    static bool generate(Chunk& chunk, JavaRandom& rand, int x, int y, int z) noexcept {
        const int height = rand.next_int(20) + 10;
        if (x < 2 || x >= 14 || z < 2 || z >= 14 || y < 1 || y + height + 2 > 255) return false;

        const auto soil = chunk.block_at(static_cast<std::size_t>(x), y - 1, static_cast<std::size_t>(z));
        if (soil != kStateGrass && soil != kStateDirt) return false;

        for (int cy = 0; cy < height; ++cy) {
            for (int dx = 0; dx <= 1; ++dx) {
                for (int dz = 0; dz <= 1; ++dz) {
                    chunk.set_block(static_cast<std::size_t>(x + dx), y + cy, static_cast<std::size_t>(z + dz), kStateJungleLog);
                }
            }
        }

        const int top_y = y + height;
        for (int cy = top_y - 2; cy <= top_y + 1; ++cy) {
            const int r = cy >= top_y ? 2 : 3;
            for (int cx = x - r; cx <= x + 1 + r; ++cx) {
                for (int cz = z - r; cz <= z + 1 + r; ++cz) {
                    if (cx >= 0 && cx < 16 && cz >= 0 && cz < 16 && cy < 256) {
                        if (chunk.block_at(static_cast<std::size_t>(cx), cy, static_cast<std::size_t>(cz)) == kStateAir) {
                            chunk.set_block(static_cast<std::size_t>(cx), cy, static_cast<std::size_t>(cz), kStateJungleLeaves);
                        }
                    }
                }
            }
        }
        return true;
    }
};

// 1.12.2 原版 2x2 巨型红杉树生成器（WorldGenMegaPineTree）
class WorldGenMegaPineTree {
public:
    static bool generate(Chunk& chunk, JavaRandom& rand, int x, int y, int z) noexcept {
        const int height = rand.next_int(15) + 15;
        if (x < 2 || x >= 14 || z < 2 || z >= 14 || y < 1 || y + height + 2 > 255) return false;

        const auto soil = chunk.block_at(static_cast<std::size_t>(x), y - 1, static_cast<std::size_t>(z));
        if (soil != kStateGrass && soil != kStateDirt && soil != kStatePodzol) return false;

        for (int cy = 0; cy < height; ++cy) {
            for (int dx = 0; dx <= 1; ++dx) {
                for (int dz = 0; dz <= 1; ++dz) {
                    chunk.set_block(static_cast<std::size_t>(x + dx), y + cy, static_cast<std::size_t>(z + dz), kStateSpruceLog);
                }
            }
        }

        const int top_y = y + height;
        for (int cy = top_y - 8; cy <= top_y + 1; ++cy) {
            const int r = std::max(1, 3 - (top_y - cy) / 3);
            for (int cx = x - r; cx <= x + 1 + r; ++cx) {
                for (int cz = z - r; cz <= z + 1 + r; ++cz) {
                    if (cx >= 0 && cx < 16 && cz >= 0 && cz < 16 && cy < 256) {
                        if (chunk.block_at(static_cast<std::size_t>(cx), cy, static_cast<std::size_t>(cz)) == kStateAir) {
                            chunk.set_block(static_cast<std::size_t>(cx), cy, static_cast<std::size_t>(cz), kStateSpruceLeaves);
                        }
                    }
                }
            }
        }
        return true;
    }
};

}  // namespace cyane::world::gen
