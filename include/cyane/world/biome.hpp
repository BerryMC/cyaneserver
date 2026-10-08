#pragma once

#include <cstdint>
#include <string_view>

#include "cyane/world/blocks.hpp"

namespace cyane::world::biome {

// 1.12.2 生物群系属性子集（仅地形生成需要的字段）
// 原版 Biome.getBaseHeight() / getHeightVariation() / topBlock / fillerBlock
struct BiomeInfo {
    std::int32_t id{0};          // 原版 Biome.getIdForBiome()（注册表 id）
    std::string_view name;       // 原版 ResourceLocation（minecraft:plains 等）
    float base_height{0.1f};     // Biome.getBaseHeight()
    float height_variation{0.2f}; // Biome.getHeightVariation()
    std::uint16_t top_block{};   // Biome.topBlock（默认 grass）
    std::uint16_t filler_block{}; // Biome.fillerBlock（默认 dirt）
};

// 常用生物群系（原版 Biomes.register 的子集）
// ID 来自原版 Biome.REGISTRY 注册顺序
inline constexpr BiomeInfo kOcean    {0,  "ocean",          -1.0f, 0.1f, kStateGravel, kStateGravel};
inline constexpr BiomeInfo kPlains   {1,  "plains",          0.125f, 0.05f, kStateGrass, kStateDirt};
inline constexpr BiomeInfo kDesert   {2,  "desert",          0.125f, 0.05f, kStateSand, kStateSand};
inline constexpr BiomeInfo kHills    {3,  "extreme_hills",   1.0f, 0.5f, kStateStone, kStateDirt};
inline constexpr BiomeInfo kForest   {4,  "forest",          0.1f, 0.2f, kStateGrass, kStateDirt};
inline constexpr BiomeInfo kTaiga    {5,  "taiga",           0.2f, 0.2f, kStateGrass, kStateDirt};
inline constexpr BiomeInfo kSwamp    {6,  "swampland",      -0.2f, 0.1f, kStateGrass, kStateDirt};
inline constexpr BiomeInfo kRiver    {7,  "river",           -0.5f, 0.0f, kStateSand, kStateSand};
inline constexpr BiomeInfo kBeach    {16, "beaches",         0.0f, 0.025f, kStateSand, kStateSand};
inline constexpr BiomeInfo kStoneBeach{25, "stone_beach",    0.1f, 0.3f, kStateStone, kStateStone};
inline constexpr BiomeInfo kSavanna  {35, "savanna",         0.125f, 0.05f, kStateGrass, kStateDirt};
inline constexpr BiomeInfo kJungle   {21, "jungle",          0.125f, 0.2f, kStateGrass, kStateDirt};
inline constexpr BiomeInfo kSnow     {12, "ice_flats",       0.125f, 0.05f, kStateSnow, kStateDirt};
inline constexpr BiomeInfo kBirchForest{27, "birch_forest",  0.1f, 0.2f, kStateGrass, kStateDirt};
inline constexpr BiomeInfo kRoofedForest{29, "roofed_forest",0.1f, 0.2f, kStateGrass, kStateDirt};
inline constexpr BiomeInfo kMesa     {39, "mesa",           0.125f, 0.05f, static_cast<std::uint16_t>(179 << 4), static_cast<std::uint16_t>(179 << 4)};

// 默认生物群系：平原
inline constexpr BiomeInfo kDefault = kPlains;

// 简化版生物群系选择器：基于坐标返回一个生物群系
// 原版使用 BiomeProvider（基于多层 Voronoi 噪声）。
// 此处使用简化版本：基于 Perlin 噪声选择基础群系。
[[nodiscard]] inline BiomeInfo biome_at(std::int32_t wx, std::int32_t wz, double noise) noexcept {
    // noise 范围约 -1..1
    if (noise < -0.5) return kOcean;
    if (noise < -0.3) return kBeach;
    if (noise < -0.15) return kRiver;
    if (noise > 0.4) return kHills;
    if (noise > 0.2) return kForest;
    if (noise > 0.05) return kTaiga;
    // 温度检查（简化：用坐标偏移模拟）
    const double temp = std::sin(static_cast<double>(wx) * 0.01) * std::cos(static_cast<double>(wz) * 0.01);
    if (temp < -0.5) return kSnow;
    if (temp > 0.5) return kDesert;
    return kPlains;
}

}  // namespace cyane::world::biome
