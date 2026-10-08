#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

#include "cyane/world/biome.hpp"
#include "cyane/world/blocks.hpp"
#include "cyane/world/chunk.hpp"
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

    // 生成一个 16×256×16 的区块（逐行转写 generateHeightmap + setBlocksInChunk + caves）
    [[nodiscard]] Chunk generate(int x, int z) {
        Chunk chunk{ChunkPos{x, z}};
        set_blocks_in_chunk(x, z, chunk);
        replace_biome_blocks(x, z, chunk);
        if (settings_.use_caves) {
            cave_generator_.generate(seed_, x, z, chunk);
        }
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
                    static_cast<double>(bx) * 0.0125, static_cast<double>(bz) * 0.0125);
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
                const double noise_val = surface_noise_->populate_noise_array_bilinear(
                    static_cast<double>(wx) * 0.0625, static_cast<double>(wz) * 0.0625);
                const auto b = biome::biome_at(wx, wz, noise_val * 0.5);

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
};

}  // namespace cyane::world::gen
