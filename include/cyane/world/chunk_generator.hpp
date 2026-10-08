#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "cyane/world/biome.hpp"
#include "cyane/world/blocks.hpp"
#include "cyane/world/chunk.hpp"
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

// ChunkGeneratorOverworld（逐行转写，仅地形噪声+地表替换，不含结构生成器）
class ChunkGeneratorOverworld {
public:
    ChunkGeneratorOverworld(std::uint64_t seed) : seed_(seed) {
        JavaRandom rng(seed);
        min_limit_noise_ = std::make_unique<NoiseGeneratorOctaves>(rng, 16);
        max_limit_noise_ = std::make_unique<NoiseGeneratorOctaves>(rng, 16);
        main_noise_ = std::make_unique<NoiseGeneratorOctaves>(rng, 8);
        surface_noise_ = std::make_unique<NoiseGeneratorImproved>(rng);
        depth_noise_ = std::make_unique<NoiseGeneratorOctaves>(rng, 16);
        height_map_.assign(825, 0.0);
    }

    // 生成一个 16×256×16 的区块（逐行转写 generateHeightmap + setBlocksInChunk）
    [[nodiscard]] Chunk generate(int x, int z) {
        Chunk chunk{ChunkPos{x, z}};
        generate_heightmap(x, z);
        set_blocks_in_chunk(chunk);
        replace_biome_blocks(x, z, chunk);
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
    std::vector<double> height_map_{825, 0.0};
    std::vector<double> depth_region_;
    std::vector<double> main_noise_region_;
    std::vector<double> min_limit_region_;
    std::vector<double> max_limit_region_;
    std::vector<double> depth_buffer_{256, 0.0};
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

    void generate_heightmap(int x, int z) {
        depth_region_ = depth_noise_->generate_noise_octaves_2d(
            depth_region_, static_cast<double>(x), static_cast<double>(z), 5, 5,
            static_cast<double>(settings_.depth_noise_scale_x),
            static_cast<double>(settings_.depth_noise_scale_z),
            static_cast<double>(settings_.depth_noise_scale_exponent));
        const double f = static_cast<double>(settings_.coordinate_scale);
        const double f1 = static_cast<double>(settings_.height_scale);
        main_noise_region_ = main_noise_->generate_noise_octaves(
            main_noise_region_, x, 0, z, 5, 33, 5,
            f / static_cast<double>(settings_.main_noise_scale_x),
            f1 / static_cast<double>(settings_.main_noise_scale_y),
            f / static_cast<double>(settings_.main_noise_scale_z));
        min_limit_region_ = min_limit_noise_->generate_noise_octaves(
            min_limit_region_, x, 0, z, 5, 33, 5, f, f1, f);
        max_limit_region_ = max_limit_noise_->generate_noise_octaves(
            max_limit_region_, x, 0, z, 5, 33, 5, f, f1, f);

        // 获取 10×10 的生物群系网格
        std::array<biome::BiomeInfo, 100> biomes{};
        for (int i = 0; i < 10; ++i) {
            for (int j = 0; j < 10; ++j) {
                const auto b_idx = static_cast<std::size_t>(i + j * 10);
                const double n = surface_noise_->populate_noise_array_bilinear(
                    static_cast<double>(x * 16 + i * 4 - 8) * 0.0125,
                    static_cast<double>(z * 16 + j * 4 - 8) * 0.0125);
                biomes[b_idx] = biome::biome_at(x * 16 + i * 4, z * 16 + j * 4, n);
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

    void set_blocks_in_chunk(Chunk& chunk) {
        std::size_t i = 0;
        for (int j2 = 0; j2 < 4; ++j2) {
            for (int l2 = 0; l2 < 4; ++l2) {
                for (int i2 = 0; i2 < 32; ++i2) {
                    double d1 = height_map_[i] * 0.25;
                    double d2 = height_map_[i + 1] * 0.25;
                    double d3 = height_map_[i + 33] * 0.25;
                    double d4 = height_map_[i + 34] * 0.25;
                    double d5 = (height_map_[i] - d1) * 0.125;
                    double d6 = (height_map_[i + 1] - d2) * 0.125;
                    double d7 = (height_map_[i + 33] - d3) * 0.125;
                    double d8 = (height_map_[i + 34] - d4) * 0.125;
                    for (int j = 0; j < 8; ++j) {
                        double d10 = d1;
                        double d11 = d2;
                        double d12 = (d3 - d1) * 0.25;
                        double d13 = (d4 - d2) * 0.25;
                        for (int k = 0; k < 4; ++k) {
                            double lvt = d10 - (d11 - d10) * 0.25;
                            for (int l = 0; l < 4; ++l) {
                                lvt += (d11 - d10) * 0.25;
                                if (lvt > 0.0) {
                                    chunk.set_block(static_cast<std::size_t>(j2 * 4 + k), i2 * 8 + j, static_cast<std::size_t>(l2 * 4 + l), kStateStone);
                                } else if (i2 * 8 + j < settings_.sea_level) {
                                    chunk.set_block(static_cast<std::size_t>(j2 * 4 + k), i2 * 8 + j, static_cast<std::size_t>(l2 * 4 + l), kStateWater);
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
                    ++i;
                }
            }
        }
    }

    void replace_biome_blocks(int x, int z, Chunk& chunk) {
        // surfaceNoise.getRegion(depthBuffer, x*16, z*16, 16, 16, 0.0625, 0.0625, 1.0)
        // 简化：用 Perlin noise 做地表起伏
        for (std::size_t i = 0; i < 16; ++i) {
            for (std::size_t j = 0; j < 16; ++j) {
                // 找地表高度（最高非空气方块）
                int surface_y = settings_.sea_level;
                for (int y = 255; y >= 0; --y) {
                    auto s = chunk.block_at(i, y, j);
                    if (s != kStateAir && block_id(s) != 8 && block_id(s) != 9) {
                        surface_y = y;
                        break;
                    }
                }
                // 获取生物群系
                double n = surface_noise_->populate_noise_array_bilinear(
                    (static_cast<double>(x * 16) + static_cast<double>(i)) * 0.0125,
                    (static_cast<double>(z * 16) + static_cast<double>(j)) * 0.0125);
                auto b = biome::biome_at(static_cast<std::int32_t>(x * 16 + static_cast<int>(i)),
                                         static_cast<std::int32_t>(z * 16 + static_cast<int>(j)), n);
                // 地表替换：顶层 top_block，下方 filler_block 3 格
                if (chunk.block_at(i, surface_y, j) == kStateStone) {
                    chunk.set_block(i, surface_y, j, b.top_block);
                    for (int dy = 1; dy <= 3 && surface_y - dy >= 0; ++dy) {
                        if (chunk.block_at(i, surface_y - dy, j) == kStateStone) {
                            chunk.set_block(i, surface_y - dy, j, b.filler_block);
                        }
                    }
                }
                // 基岩层（y=0）
                chunk.set_block(i, 0, j, kStateBedrock);
            }
        }
    }
};

}  // namespace cyane::world::gen
