#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

#include "cyane/world/biome.hpp"
#include "cyane/world/blocks.hpp"
#include "cyane/world/chunk.hpp"
#include "cyane/world/feature.hpp"
#include "cyane/world/map_gen.hpp"
#include "cyane/world/noise.hpp"

namespace cyane::world::gen {

// ChunkGeneratorSettings（原版默认值，逐字段对应）
struct GeneratorSettings {
    float coordinate_scale = 684.412f;
    float height_scale = 684.412f;
    float upper_limit_scale = 512.0f;
    float lower_limit_scale = 512.0f;
    float depth_noise_scale_x = 200.0f;
    float depth_noise_scale_z = 200.0f;
    float depth_noise_scale_exponent = 0.5f;
    float main_noise_scale_x = 80.0f;
    float main_noise_scale_y = 160.0f;
    float main_noise_scale_z = 80.0f;
    float base_size = 8.5f;
    float stretch_y = 12.0f;
    float biome_depth_weight = 1.0f;
    float biome_depth_offset = 0.0f;
    float biome_scale_weight = 1.0f;
    float biome_scale_offset = 0.0f;
    int sea_level = 63;
    bool use_caves = true;
    bool use_ravines = true;
};

// ChunkGeneratorOverworld（逐行转写 1.12.2 原版）
class ChunkGeneratorOverworld {
public:
    explicit ChunkGeneratorOverworld(std::uint64_t seed) : seed_(seed) {
        JavaRandom rng(seed);
        min_limit_noise_ = std::make_unique<NoiseGeneratorOctaves>(rng, 16);
        max_limit_noise_ = std::make_unique<NoiseGeneratorOctaves>(rng, 16);
        main_noise_ = std::make_unique<NoiseGeneratorOctaves>(rng, 8);
        surface_noise_ = std::make_unique<NoiseGeneratorImproved>(rng);
        depth_noise_ = std::make_unique<NoiseGeneratorOctaves>(rng, 16);
        height_map_.assign(825, 0.0);
    }

    // 生成一个 16×256×16 的区块（逐行转写 generateHeightmap + setBlocksInChunk + caves + ores + decorations）
    [[nodiscard]] Chunk generate(int x, int z) {
        Chunk chunk{ChunkPos{x, z}};
        set_blocks_in_chunk(x, z, chunk);
        replace_biome_blocks(x, z, chunk);
        if (settings_.use_caves) {
            cave_generator_.generate(seed_, x, z, chunk);
        }
        JavaRandom rand(seed_ ^ (static_cast<std::uint64_t>(x) * 341873128712ULL +
                                 static_cast<std::uint64_t>(z) * 132897987541ULL));
        generate_ores(x, z, chunk, rand);
        generate_decorations(x, z, chunk, rand);
        return chunk;
    }

    [[nodiscard]] std::uint64_t seed() const noexcept { return seed_; }

private:
    std::uint64_t seed_{0};
    std::unique_ptr<NoiseGeneratorOctaves> min_limit_noise_;
    std::unique_ptr<NoiseGeneratorOctaves> max_limit_noise_;
    std::unique_ptr<NoiseGeneratorOctaves> main_noise_;
    std::unique_ptr<NoiseGeneratorImproved> surface_noise_;
    std::unique_ptr<NoiseGeneratorOctaves> depth_noise_;
    MapGenCaves cave_generator_;
    std::vector<double> height_map_{825, 0.0};
    std::vector<double> depth_region_;
    std::vector<double> main_noise_region_;
    std::vector<double> min_limit_region_;
    std::vector<double> max_limit_region_;
    GeneratorSettings settings_;
    static constexpr double kBiomeNoiseScale = 0.0005;

    // biomeWeights[5×5]（对齐原版 ChunkGeneratorOverworld 构造器）
    static const std::array<float, 25>& biome_weights() {
        static const std::array<float, 25> w = [] {
            std::array<float, 25> w{};
            for (int i = -2; i <= 2; ++i) {
                for (int j = -2; j <= 2; ++j) {
                    const auto idx = static_cast<std::size_t>((i + 2) + (j + 2) * 5);
                    const auto dist_sq = static_cast<float>(i * i + j * j) + 0.2f;
                    w[idx] = 10.0f / std::sqrt(dist_sq);
                }
            }
            return w;
        }();
        return w;
    }

    void generate_heightmap(int x, int y, int z) {
        depth_region_ = depth_noise_->generate_noise_octaves_2d(
            depth_region_, static_cast<double>(x), static_cast<double>(z), 5, 5,
            static_cast<double>(settings_.depth_noise_scale_x),
            static_cast<double>(settings_.depth_noise_scale_z),
            static_cast<double>(settings_.depth_noise_scale_exponent));
        const double f = static_cast<double>(settings_.coordinate_scale);
        const double f1 = static_cast<double>(settings_.height_scale);
        main_noise_region_ = main_noise_->generate_noise_octaves(
            main_noise_region_, x, y, z, 5, 33, 5,
            f / static_cast<double>(settings_.main_noise_scale_x),
            f1 / static_cast<double>(settings_.main_noise_scale_y),
            f / static_cast<double>(settings_.main_noise_scale_z));
        min_limit_region_ = min_limit_noise_->generate_noise_octaves(
            min_limit_region_, x, y, z, 5, 33, 5, f, f1, f);
        max_limit_region_ = max_limit_noise_->generate_noise_octaves(
            max_limit_region_, x, y, z, 5, 33, 5, f, f1, f);

        // 获取 10×10 的生物群系网格（在 x - 2, z - 2 采样，4 格一个点）
        std::array<biome::BiomeInfo, 100> biomes{};
        for (int i = 0; i < 10; ++i) {
            for (int j = 0; j < 10; ++j) {
                const auto b_idx = static_cast<std::size_t>(i + j * 10);
                const int bx = (x - 2 + i) * 4;
                const int bz = (z - 2 + j) * 4;
                const double n = surface_noise_->populate_noise_array_bilinear(
                    static_cast<double>(bx) * kBiomeNoiseScale, static_cast<double>(bz) * kBiomeNoiseScale);
                biomes[b_idx] = biome::biome_at(bx, bz, n);
            }
        }

        std::size_t idx = 0;
        std::size_t j = 0;
        for (int k = 0; k < 5; ++k) {
            for (int l = 0; l < 5; ++l) {
                float f2 = 0.0f;
                float f3 = 0.0f;
                float f4 = 0.0f;
                const auto center_idx = static_cast<std::size_t>(k + 2 + (l + 2) * 10);
                const auto& biome_center = biomes[center_idx];
                for (int j1 = -2; j1 <= 2; ++j1) {
                    for (int k1 = -2; k1 <= 2; ++k1) {
                        const auto b_idx = static_cast<std::size_t>(k + j1 + 2 + (l + k1 + 2) * 10);
                        const auto& b = biomes[b_idx];
                        const float f5 = settings_.biome_depth_offset + b.base_height * settings_.biome_depth_weight;
                        const float f6 = settings_.biome_scale_offset + b.height_variation * settings_.biome_scale_weight;
                        const auto w_idx = static_cast<std::size_t>((j1 + 2) + (k1 + 2) * 5);
                        float f7 = biome_weights()[w_idx] / (f5 + 2.0f);
                        if (b.base_height > biome_center.base_height) {
                            f7 /= 2.0f;
                        }
                        f2 += f6 * f7;
                        f3 += f5 * f7;
                        f4 += f7;
                    }
                }
                f2 /= f4;
                f3 /= f4;
                f2 = f2 * 0.9f + 0.1f;
                f3 = (f3 * 4.0f - 1.0f) / 8.0f;

                double d7 = depth_region_[j] / 8000.0;
                if (d7 < 0.0) d7 = -d7 * 0.3;
                d7 = d7 * 3.0 - 2.0;
                if (d7 < 0.0) {
                    d7 /= 2.0;
                    if (d7 < -1.0) d7 = -1.0;
                    d7 /= 1.4;
                    d7 /= 2.0;
                } else {
                    if (d7 > 1.0) d7 = 1.0;
                    d7 /= 8.0;
                }
                ++j;
                const double d8 = (static_cast<double>(f3) + d7 * 0.2) * static_cast<double>(settings_.base_size) / 8.0;
                const double d0 = static_cast<double>(settings_.base_size) + d8 * 4.0;
                for (int l1 = 0; l1 < 33; ++l1) {
                    double d1 = (static_cast<double>(l1) - d0) * static_cast<double>(settings_.stretch_y) * 128.0 / 256.0 / static_cast<double>(f2);
                    if (d1 < 0.0) d1 *= 4.0;
                    const double d2 = min_limit_region_[idx] / static_cast<double>(settings_.lower_limit_scale);
                    const double d3 = max_limit_region_[idx] / static_cast<double>(settings_.upper_limit_scale);
                    const double d4 = (main_noise_region_[idx] / 10.0 + 1.0) / 2.0;
                    double d5 = clamped_lerp(d2, d3, d4) - d1;
                    if (l1 > 29) {
                        const double d6 = static_cast<double>(l1 - 29) / 3.0;
                        d5 = d5 * (1.0 - d6) + -10.0 * d6;
                    }
                    height_map_[idx] = d5;
                    ++idx;
                }
            }
        }
    }

    [[nodiscard]] static double clamped_lerp(double a, double b, double t) noexcept {
        if (t <= 0.0) return a;
        if (t >= 1.0) return b;
        return a + (b - a) * t;
    }

    void set_blocks_in_chunk(int x, int z, Chunk& chunk) {
        generate_heightmap(x * 4, 0, z * 4);

        for (int i = 0; i < 4; ++i) {
            const int j = i * 5;
            const int k = (i + 1) * 5;

            for (int l = 0; l < 4; ++l) {
                const int i1 = (j + l) * 33;
                const int j1 = (j + l + 1) * 33;
                const int k1 = (k + l) * 33;
                const int l1 = (k + l + 1) * 33;

                for (int i2 = 0; i2 < 32; ++i2) {
                    constexpr double d0 = 0.125;
                    double d1 = height_map_[static_cast<std::size_t>(i1 + i2)];
                    double d2 = height_map_[static_cast<std::size_t>(j1 + i2)];
                    double d3 = height_map_[static_cast<std::size_t>(k1 + i2)];
                    double d4 = height_map_[static_cast<std::size_t>(l1 + i2)];
                    const double d5 = (height_map_[static_cast<std::size_t>(i1 + i2 + 1)] - d1) * d0;
                    const double d6 = (height_map_[static_cast<std::size_t>(j1 + i2 + 1)] - d2) * d0;
                    const double d7 = (height_map_[static_cast<std::size_t>(k1 + i2 + 1)] - d3) * d0;
                    const double d8 = (height_map_[static_cast<std::size_t>(l1 + i2 + 1)] - d4) * d0;

                    for (int j2 = 0; j2 < 8; ++j2) {
                        constexpr double d9 = 0.25;
                        double d10 = d1;
                        double d11 = d2;
                        const double d12 = (d3 - d1) * d9;
                        const double d13 = (d4 - d2) * d9;

                        for (int k2 = 0; k2 < 4; ++k2) {
                            const double d16 = (d11 - d10) * d9;
                            double lvt_45_1 = d10 - d16;

                            for (int l2 = 0; l2 < 4; ++l2) {
                                lvt_45_1 += d16;
                                const auto bx = static_cast<std::size_t>(i * 4 + k2);
                                const auto by = static_cast<std::int32_t>(i2 * 8 + j2);
                                const auto bz = static_cast<std::size_t>(l * 4 + l2);

                                if (lvt_45_1 > 0.0) {
                                    chunk.set_block(bx, by, bz, kStateStone);
                                } else if (by < settings_.sea_level) {
                                    chunk.set_block(bx, by, bz, kStateWater);
                                }
                            }

                            d10 += d12;
                            d11 += d13;
                        }

                        d1 += d5;
                        d2 += d6;
                        d3 += d7;
                        d4 += d8;
                    }
                }
            }
        }
    }

    void replace_biome_blocks(int x, int z, Chunk& chunk) {
        JavaRandom rng(static_cast<std::uint64_t>(x) * 341873128712ULL +
                       static_cast<std::uint64_t>(z) * 132897987541ULL);
        const int sea_level = settings_.sea_level;

        for (std::size_t i = 0; i < 16; ++i) {
            for (std::size_t j = 0; j < 16; ++j) {
                const int wx = x * 16 + static_cast<int>(i);
                const int wz = z * 16 + static_cast<int>(j);
                const double biome_noise = surface_noise_->populate_noise_array_bilinear(
                    static_cast<double>(wx) * kBiomeNoiseScale, static_cast<double>(wz) * kBiomeNoiseScale);
                const auto b = biome::biome_at(wx, wz, biome_noise);

                const double noise_val = surface_noise_->populate_noise_array_bilinear(
                    static_cast<double>(wx) * 0.0625, static_cast<double>(wz) * 0.0625);

                const int k = static_cast<int>(noise_val / 3.0 + 3.0 + rng.next_double() * 0.25);
                int depth = -1;
                std::uint16_t top = b.top_block;
                std::uint16_t filler = b.filler_block;

                for (int y = 255; y >= 0; --y) {
                    if (y <= rng.next_int(5)) {
                        chunk.set_block(i, y, j, kStateBedrock);
                    } else {
                        const auto current = chunk.block_at(i, y, j);
                        if (current == kStateAir) {
                            depth = -1;
                        } else if (current == kStateStone) {
                            if (depth == -1) {
                                if (k <= 0) {
                                    top = kStateAir;
                                    filler = kStateStone;
                                } else if (y >= sea_level - 4 && y <= sea_level + 1) {
                                    top = b.top_block;
                                    filler = b.filler_block;
                                }

                                depth = k;
                                if (y >= sea_level - 1) {
                                    chunk.set_block(i, y, j, top);
                                } else if (y < sea_level - 7 - k) {
                                    chunk.set_block(i, y, j, kStateGravel);
                                } else {
                                    chunk.set_block(i, y, j, filler);
                                }
                            } else if (depth > 0) {
                                --depth;
                                chunk.set_block(i, y, j, filler);
                            }
                        }
                    }
                }
            }
        }
    }

    void generate_ores(int x, int z, Chunk& chunk, JavaRandom& rand) {
        const double noise_val = surface_noise_->populate_noise_array_bilinear(
            static_cast<double>(x * 16 + 8) * 0.0625, static_cast<double>(z * 16 + 8) * 0.0625);
        const auto b = biome::biome_at(x * 16 + 8, z * 16 + 8, noise_val * 0.5);

        static constexpr WorldGenMinable dirt_gen{kStateDirt, 33};
        static constexpr WorldGenMinable gravel_gen{kStateGravel, 33};
        static constexpr WorldGenMinable granite_gen{kStateGranite, 33};
        static constexpr WorldGenMinable diorite_gen{kStateDiorite, 33};
        static constexpr WorldGenMinable andesite_gen{kStateAndesite, 33};
        static constexpr WorldGenMinable coal_gen{kStateCoalOre, 17};
        static constexpr WorldGenMinable iron_gen{kStateIronOre, 9};
        static constexpr WorldGenMinable gold_gen{kStateGoldOre, 9};
        static constexpr WorldGenMinable redstone_gen{kStateRedstoneOre, 8};
        static constexpr WorldGenMinable diamond_gen{kStateDiamondOre, 8};
        static constexpr WorldGenMinable lapis_gen{kStateLapisOre, 7};

        const auto gen_ore = [&](int count, const WorldGenMinable& gen, int min_y, int max_y) {
            for (int i = 0; i < count; ++i) {
                const int px = rand.next_int(16);
                const int py = rand.next_int(max_y - min_y) + min_y;
                const int pz = rand.next_int(16);
                gen.generate(chunk, rand, px, py, pz);
            }
        };

        gen_ore(10, dirt_gen, 0, 256);
        gen_ore(8, gravel_gen, 0, 256);
        gen_ore(10, granite_gen, 0, 80);
        gen_ore(10, diorite_gen, 0, 80);
        gen_ore(10, andesite_gen, 0, 80);
        gen_ore(20, coal_gen, 0, 128);
        gen_ore(20, iron_gen, 0, 64);
        gen_ore(2, gold_gen, 0, 32);
        gen_ore(8, redstone_gen, 0, 16);
        gen_ore(1, diamond_gen, 0, 16);

        // 青金石（中心高度 16，扩散 16）
        for (int i = 0; i < 1; ++i) {
            const int px = rand.next_int(16);
            const int py = rand.next_int(16) + rand.next_int(16) + 16 - 16;
            const int pz = rand.next_int(16);
            if (py >= 0 && py < 256) {
                lapis_gen.generate(chunk, rand, px, py, pz);
            }
        }

        // 绿宝石矿（原版仅极限山岭 4..32 层出现，每区块 3..8 次独立单方块矿脉）
        if (b.id == 3) {
            static constexpr WorldGenMinable emerald_gen{kStateEmeraldOre, 1};
            const int attempts = rand.next_int(6) + 3;
            for (int i = 0; i < attempts; ++i) {
                const int px = rand.next_int(16);
                const int py = rand.next_int(28) + 4;
                const int pz = rand.next_int(16);
                emerald_gen.generate(chunk, rand, px, py, pz);
            }
        }
    }

    void generate_decorations(int x, int z, Chunk& chunk, JavaRandom& rand) {
        const double biome_noise = surface_noise_->populate_noise_array_bilinear(
            static_cast<double>(x * 16 + 8) * kBiomeNoiseScale, static_cast<double>(z * 16 + 8) * kBiomeNoiseScale);
        const auto b = biome::biome_at(x * 16 + 8, z * 16 + 8, biome_noise);

        // 1. 水湖 (1/4 几率且非沙漠) 与 地下熔岩湖 (1/80 几率)
        if (b.id != 2 && rand.next_int(4) == 0) {
            static constexpr WorldGenLakes water_lake{kStateWater};
            const int lx = rand.next_int(16);
            const int lz = rand.next_int(16);
            int ly = 0;
            for (int y = 120; y >= 50; --y) {
                if (chunk.block_at(static_cast<std::size_t>(lx), y, static_cast<std::size_t>(lz)) != kStateAir) {
                    ly = y;
                    break;
                }
            }
            if (ly >= 50) {
                water_lake.generate(chunk, rand, lx, ly, lz);
            }
        }
        if (rand.next_int(80) == 0) {
            static constexpr WorldGenLakes lava_lake{kStateLava};
            const int lx = rand.next_int(16);
            const int ly = rand.next_int(48) + 8;
            const int lz = rand.next_int(16);
            lava_lake.generate(chunk, rand, lx, ly, lz);
        }

        // 2. 地下岩壁泉眼（水瀑布与熔岩泉）
        static constexpr WorldGenLiquids water_spring{kStateWater};
        static constexpr WorldGenLiquids lava_spring{kStateLava};
        for (int i = 0; i < 50; ++i) {
            const int sx = rand.next_int(14) + 1;
            const int sy = rand.next_int(rand.next_int(240) + 8) + 4;
            const int sz = rand.next_int(14) + 1;
            water_spring.generate(chunk, sx, sy, sz);
        }
        for (int i = 0; i < 20; ++i) {
            const int sx = rand.next_int(14) + 1;
            const int sy = rand.next_int(rand.next_int(rand.next_int(230) + 8) + 8) + 4;
            const int sz = rand.next_int(14) + 1;
            lava_spring.generate(chunk, sx, sy, sz);
        }

        // 3. 树木生成
        int trees_per_chunk = 0;
        int grass_per_chunk = 1;
        int flowers_per_chunk = 1;
        switch (b.id) {
            case 4: case 29: // 森林 / 黑森林
                trees_per_chunk = 10;
                grass_per_chunk = 2;
                flowers_per_chunk = 2;
                break;
            case 27: // 白桦林
                trees_per_chunk = 10;
                grass_per_chunk = 2;
                flowers_per_chunk = 2;
                break;
            case 1: // 平原
                trees_per_chunk = rand.next_float() < 0.05f ? 1 : 0;
                grass_per_chunk = 10;
                flowers_per_chunk = 4;
                break;
            case 3: // 山地
                trees_per_chunk = 2;
                break;
            case 5: case 30: case 32: // 针叶林系列
                trees_per_chunk = 8;
                break;
            case 6: // 沼泽
                trees_per_chunk = 2;
                grass_per_chunk = 5;
                break;
            case 35: case 36: // 热带草原系列
                trees_per_chunk = 1;
                grass_per_chunk = 8;
                break;
            case 21: // 丛林
                trees_per_chunk = 12;
                grass_per_chunk = 12;
                flowers_per_chunk = 4;
                break;
            default:
                break;
        }

        static const WorldGenTrees oak_tree{4, kStateOakLog, kStateOakLeaves};
        static const WorldGenTrees birch_tree{5, kStateBirchLog, kStateBirchLeaves};
        for (int i = 0; i < trees_per_chunk; ++i) {
            const int tx = rand.next_int(12) + 2;
            const int tz = rand.next_int(12) + 2;
            int ty = 0;
            for (int y = 250; y >= 60; --y) {
                const auto state = chunk.block_at(static_cast<std::size_t>(tx), y, static_cast<std::size_t>(tz));
                if (state == kStateGrass || state == kStateDirt || state == kStatePodzol) {
                    ty = y + 1;
                    break;
                }
            }
            if (ty >= 60) {
                if (b.id == 27) {
                    birch_tree.generate(chunk, rand, tx, ty, tz);
                } else if (b.id == 5 || b.id == 30 || b.id == 32) {
                    WorldGenTaiga1::generate(chunk, rand, tx, ty, tz);
                } else if (b.id == 35 || b.id == 36) {
                    WorldGenSavannaTree::generate(chunk, rand, tx, ty, tz);
                } else {
                    oak_tree.generate(chunk, rand, tx, ty, tz);
                }
            }
        }

        // 4. 特色植物与作物
        // 甘蔗（沿水边泥土/沙地上生成）
        if (b.id != 2) {
            const int rx = rand.next_int(16);
            const int rz = rand.next_int(16);
            for (int y = 120; y >= 50; --y) {
                const auto s = chunk.block_at(static_cast<std::size_t>(rx), y, static_cast<std::size_t>(rz));
                if (s == kStateGrass || s == kStateDirt || s == kStateSand) {
                    WorldGenReed::generate(chunk, rand, rx, y + 1, rz);
                    break;
                }
            }
        }

        // 仙人掌（仅沙漠生成）
        if (b.id == 2) {
            for (int i = 0; i < 2; ++i) {
                const int cx = rand.next_int(16);
                const int cz = rand.next_int(16);
                for (int y = 120; y >= 50; --y) {
                    if (chunk.block_at(static_cast<std::size_t>(cx), y, static_cast<std::size_t>(cz)) == kStateSand) {
                        WorldGenCactus::generate(chunk, rand, cx, y + 1, cz);
                        break;
                    }
                }
            }
        }

        // 睡莲（仅沼泽水面生成）
        if (b.id == 6) {
            const int wx_pos = rand.next_int(16);
            const int wz_pos = rand.next_int(16);
            WorldGenWaterlily::generate(chunk, rand, wx_pos, settings_.sea_level, wz_pos);
        }

        // 野生南瓜（1/32 区块几率）
        if (rand.next_int(32) == 0) {
            const int px = rand.next_int(16);
            const int pz = rand.next_int(16);
            for (int y = 120; y >= 60; --y) {
                if (chunk.block_at(static_cast<std::size_t>(px), y, static_cast<std::size_t>(pz)) == kStateGrass) {
                    WorldGenPumpkin::generate(chunk, rand, px, y + 1, pz);
                    break;
                }
            }
        }

        // 5. 杂草与花朵
        for (int i = 0; i < grass_per_chunk; ++i) {
            const int gx = rand.next_int(16);
            const int gz = rand.next_int(16);
            for (int y = 250; y >= 60; --y) {
                if (chunk.block_at(static_cast<std::size_t>(gx), y, static_cast<std::size_t>(gz)) == kStateGrass) {
                    WorldGenTallGrass::generate(chunk, rand, gx, y + 1, gz);
                    break;
                }
            }
        }
        for (int i = 0; i < flowers_per_chunk; ++i) {
            const int fx = rand.next_int(16);
            const int fz = rand.next_int(16);
            const std::uint16_t flower = rand.next_int(2) == 0 ? kStateDandelion : kStatePoppy;
            for (int y = 250; y >= 60; --y) {
                if (chunk.block_at(static_cast<std::size_t>(fx), y, static_cast<std::size_t>(fz)) == kStateGrass) {
                    WorldGenFlowers::generate(chunk, rand, fx, y + 1, fz, flower);
                    break;
                }
            }
        }

        // 6. 巨型蘑菇（蘑菇岛必刷，深色橡树黑森林 1/12 几率）
        if (b.id == 14 || (b.id == 29 && rand.next_int(12) == 0)) {
            const int mx = rand.next_int(10) + 3;
            const int mz = rand.next_int(10) + 3;
            for (int y = 120; y >= 60; --y) {
                const auto s = chunk.block_at(static_cast<std::size_t>(mx), y, static_cast<std::size_t>(mz));
                if (s == kStateGrass || s == kStateDirt || s == kStateMycelium) {
                    WorldGenBigMushroom::generate(chunk, rand, mx, y + 1, mz, rand.next_int(2) == 0);
                    break;
                }
            }
        }

        // 7. 极地/寒带冰刺与冰道
        if (b.id == 12 || b.id == 30) {
            if (rand.next_int(60) == 0) {
                const int sx = rand.next_int(8) + 4;
                const int sz = rand.next_int(8) + 4;
                for (int y = 120; y >= 60; --y) {
                    if (chunk.block_at(static_cast<std::size_t>(sx), y, static_cast<std::size_t>(sz)) != kStateAir) {
                        WorldGenIceSpike::generate(chunk, rand, sx, y + 1, sz);
                        break;
                    }
                }
            }
        }

        // 8. 极地/寒带结冰与覆雪（Populate.ICE）
        if (b.id == 12 || b.id == 30 || b.id == 26) {
            for (std::size_t ix = 0; ix < 16; ++ix) {
                for (std::size_t iz = 0; iz < 16; ++iz) {
                    for (int y = 250; y >= 1; --y) {
                        const auto current = chunk.block_at(ix, y, iz);
                        if (current != kStateAir) {
                            if (current == kStateWater) {
                                chunk.set_block(ix, y, iz, kStateIce);
                            } else if (is_solid(current) && y < 255) {
                                chunk.set_block(ix, y + 1, iz, kStateSnow);
                            }
                            break;
                        }
                    }
                }
            }
        }
    }
};

}  // namespace cyane::world::gen
