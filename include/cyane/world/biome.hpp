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
inline constexpr BiomeInfo kOcean         {0,   "ocean",                -1.0f,  0.1f,   kStateGravel, kStateGravel};
inline constexpr BiomeInfo kPlains        {1,   "plains",                0.125f, 0.05f,  kStateGrass,  kStateDirt};
inline constexpr BiomeInfo kDesert        {2,   "desert",                0.125f, 0.05f,  kStateSand,   kStateSand};
inline constexpr BiomeInfo kHills         {3,   "extreme_hills",         1.0f,   0.5f,   kStateStone,  kStateDirt};
inline constexpr BiomeInfo kForest        {4,   "forest",                0.1f,   0.2f,   kStateGrass,  kStateDirt};
inline constexpr BiomeInfo kTaiga         {5,   "taiga",                 0.2f,   0.2f,   kStateGrass,  kStateDirt};
inline constexpr BiomeInfo kSwamp         {6,   "swampland",            -0.2f,  0.1f,   kStateGrass,  kStateDirt};
inline constexpr BiomeInfo kRiver         {7,   "river",                -0.5f,  0.0f,   kStateSand,   kStateSand};
inline constexpr BiomeInfo kHell          {8,   "hell",                  0.1f,   0.2f,   kStateNetherrack, kStateNetherrack};
inline constexpr BiomeInfo kSky           {9,   "sky",                   0.1f,   0.2f,   kStateEndStone, kStateEndStone};
inline constexpr BiomeInfo kSnow          {12,  "ice_flats",             0.125f, 0.05f,  kStateSnow,   kStateDirt};
inline constexpr BiomeInfo kMushroomIsland{14,  "mushroom_island",       0.2f,   0.3f,   kStateMycelium, kStateDirt};
inline constexpr BiomeInfo kMushroomIslandShore{15, "mushroom_island_shore", 0.0f, 0.025f, kStateMycelium, kStateDirt};
inline constexpr BiomeInfo kBeach         {16,  "beaches",               0.0f,   0.025f, kStateSand,   kStateSand};
inline constexpr BiomeInfo kJungle        {21,  "jungle",                0.125f, 0.2f,   kStateGrass,  kStateDirt};
inline constexpr BiomeInfo kDeepOcean     {24,  "deep_ocean",           -1.8f,  0.1f,   kStateGravel, kStateGravel};
inline constexpr BiomeInfo kStoneBeach    {25,  "stone_beach",           0.1f,   0.3f,   kStateStone,  kStateStone};
inline constexpr BiomeInfo kColdBeach     {26,  "cold_beach",            0.0f,   0.025f, kStateSand,   kStateSand};
inline constexpr BiomeInfo kBirchForest   {27,  "birch_forest",          0.1f,   0.2f,   kStateGrass,  kStateDirt};
inline constexpr BiomeInfo kRoofedForest  {29,  "roofed_forest",         0.1f,   0.2f,   kStateGrass,  kStateDirt};
inline constexpr BiomeInfo kColdTaiga     {30,  "taiga_cold",            0.2f,   0.2f,   kStateSnow,   kStateDirt};
inline constexpr BiomeInfo kRedwoodTaiga  {32,  "redwood_taiga",         0.2f,   0.2f,   kStatePodzol, kStateDirt};
inline constexpr BiomeInfo kSavanna       {35,  "savanna",               0.125f, 0.05f,  kStateGrass,  kStateDirt};
inline constexpr BiomeInfo kSavannaPlateau{36,  "savanna_rock",          1.5f,   0.025f, kStateGrass,  kStateDirt};
inline constexpr BiomeInfo kMesaPlateau   {38,  "mesa_clear_rock",       1.5f,   0.025f, static_cast<std::uint16_t>(179 << 4), static_cast<std::uint16_t>(179 << 4)};
inline constexpr BiomeInfo kMesa          {39,  "mesa",                  0.125f, 0.05f,  static_cast<std::uint16_t>(179 << 4), static_cast<std::uint16_t>(179 << 4)};
inline constexpr BiomeInfo kVoid          {127, "void",                  0.1f,   0.2f,   kStateAir,    kStateAir};
inline constexpr BiomeInfo kMutatedForest {132, "mutated_forest",        0.1f,   0.4f,   kStateGrass,  kStateDirt};
inline constexpr BiomeInfo kMutatedSavanna{163, "mutated_savanna",       0.36f,  1.225f, kStateGrass,  kStateDirt};

// 默认生物群系：平原
inline constexpr BiomeInfo kDefault = kPlains;

// 气候系统生物群系选择器：基于大陆架与大尺度温湿度分区（对齐原版 1.12.2 气候分带）
[[nodiscard]] inline BiomeInfo biome_at(std::int32_t wx, std::int32_t wz, double continentalness) noexcept {
    // 1. 大陆架高程分区（宏观大尺度海洋与海岸线）
    if (continentalness < -0.45) return kDeepOcean;
    if (continentalness < -0.15) return kOcean;
    if (continentalness < -0.05) {
        // 海岸线：根据温度分流为冷水沙滩、石滩或沙滩
        const double temp = std::sin(static_cast<double>(wx) * 0.0004) * std::cos(static_cast<double>(wz) * 0.0004);
        if (temp < -0.25) return kColdBeach;
        if (continentalness > -0.08 && temp > 0.0) return kStoneBeach;
        return kBeach;
    }

    // 2. 陆地气候带（温度与湿度大尺度连续场：周期 ~2500 方块，防止微观杂乱交错）
    const double temp = std::sin(static_cast<double>(wx) * 0.00035) * std::cos(static_cast<double>(wz) * 0.00035);
    const double humid = std::cos(static_cast<double>(wx) * 0.00045 + 1.2) * std::sin(static_cast<double>(wz) * 0.00045);

    // 极端高山（内陆高隆起区）
    if (continentalness > 0.4) {
        if (temp > 0.3) return kSavannaPlateau;
        if (temp < -0.2) return kSnow; // 积雪高山
        return kHills;
    }

    // 寒冷带 (Cold / Snowy Zone)
    if (temp < -0.2) {
        if (humid > 0.15) return kColdTaiga;
        if (humid < -0.15) return kTaiga;
        return kSnow;
    }

    // 炎热干燥带 (Hot / Dry Zone)
    if (temp > 0.25) {
        if (humid < -0.15) return kDesert;
        if (humid > 0.25) return kJungle;
        if (humid > 0.1) return kMesa;
        return kSavanna;
    }

    // 温带湿润与温和区 (Temperate Zone: Plains, Forests, Swamps)
    if (humid > 0.3) {
        return kSwamp;
    }
    if (humid > 0.1) {
        if (continentalness > 0.15) return kRoofedForest;
        return kForest;
    }
    if (humid > -0.1) {
        if (temp > 0.05) return kBirchForest;
        return kPlains;
    }

    return kPlains;
}

}  // namespace cyane::world::biome
