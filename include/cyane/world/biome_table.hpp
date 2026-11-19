#pragma once

#include <array>
#include <cstdint>
#include <optional>

// 生物群系的两张辅助表，均由原版源码直接抽取，不要手改：
//   变异体：Biome.java 里 registerBiome(...).setBaseBiome("x") 的 21 对，
//           即 getMutationForBiome = fromId(id + 128)
//   群系类：registerBiome 的 new <Class>(...) —— 对应 getBiomeClass() 的等价关系
// 两张表由 tools 脚本从 ForgeDevEnv 的 MCP 源码生成。
namespace cyane::world::biome_table {

// 有变异体的基础 id（其余 id 调用 getMutationForBiome 会得到 null）
inline constexpr std::array<std::int32_t, 21> kMutationBases{1, 2, 3, 4, 5, 6, 12, 21, 23, 27, 28, 29, 30, 32, 33, 34, 35, 36, 37, 38, 39};

[[nodiscard]] constexpr bool has_mutation(std::int32_t id) noexcept {
    for (const auto base : kMutationBases) {
        if (base == id) return true;
    }
    return false;
}

// Biome.getMutationForBiome：无变异体时返回 nullopt（原版返回 null）
[[nodiscard]] constexpr std::optional<std::int32_t> mutation_of(std::int32_t id) noexcept {
    return has_mutation(id) ? std::optional<std::int32_t>{id + 128} : std::nullopt;
}

// 变异体 id 判定（Biome.isMutation = baseBiomeRegName != null）
[[nodiscard]] constexpr bool is_mutation(std::int32_t id) noexcept {
    return id >= 128 && has_mutation(id - 128);
}

enum class BiomeClass : std::uint8_t {
    taiga,
    forest,
    mesa,
    hills,
    jungle,
    ocean,
    desert,
    snow,
    plains,
    swamp,
    river,
    mushroom,
    beach,
    savanna,
    forest_mutated,
    savanna_mutated,
    hell,
    end,
    stone_beach,
    the_void,
    unknown,
};

// Biome.getBiomeClass() 的等价类。注意 mutated_forest / mutated_savanna 是独立子类，
// 与 forest / savanna 并不等价。
[[nodiscard]] constexpr BiomeClass biome_class(std::int32_t id) noexcept {
    switch (id) {
        case 5: case 19: case 30: case 31: case 32: case 33: case 133: case 158: case 160: case 161: return BiomeClass::taiga;
        case 4: case 18: case 27: case 28: case 29: case 132: case 157: return BiomeClass::forest;
        case 37: case 38: case 39: case 165: case 166: case 167: return BiomeClass::mesa;
        case 3: case 20: case 34: case 131: case 162: return BiomeClass::hills;
        case 21: case 22: case 23: case 149: case 151: return BiomeClass::jungle;
        case 0: case 10: case 24: return BiomeClass::ocean;
        case 2: case 17: case 130: return BiomeClass::desert;
        case 12: case 13: case 140: return BiomeClass::snow;
        case 1: case 129: return BiomeClass::plains;
        case 6: case 134: return BiomeClass::swamp;
        case 7: case 11: return BiomeClass::river;
        case 14: case 15: return BiomeClass::mushroom;
        case 16: case 26: return BiomeClass::beach;
        case 35: case 36: return BiomeClass::savanna;
        case 155: case 156: return BiomeClass::forest_mutated;
        case 163: case 164: return BiomeClass::savanna_mutated;
        case 8: return BiomeClass::hell;
        case 9: return BiomeClass::end;
        case 25: return BiomeClass::stone_beach;
        case 127: return BiomeClass::the_void;
    default: return BiomeClass::unknown;
    }
}

// GenLayer.biomesEqualOrMesaPlateau。
// 注意原版是**不对称**的：只有当第一个参数是 mesa_rock/mesa_clear_rock 时才走
// "对方也必须是 mesa 两兄弟"的分支；否则一律按群系类比较。
[[nodiscard]] constexpr bool biomes_equal_or_mesa_plateau(std::int32_t a,
                                                          std::int32_t b) noexcept {
    if (a == b) return true;
    constexpr std::int32_t kMesaRock = 38;
    constexpr std::int32_t kMesaClearRock = 39;
    if (a == kMesaRock || a == kMesaClearRock) {
        return b == kMesaRock || b == kMesaClearRock;
    }
    const auto ca = biome_class(a);
    return ca != BiomeClass::unknown && ca == biome_class(b);
}

// 温度类别（Biome.getTempCategory）：defaultTemperature < 0.2 → COLD，
// < 1.0 → MEDIUM，否则 WARM。数值由 Biome.java 的 setTemperature 抽取。
enum class TempCategory : std::uint8_t { cold, medium, warm };

[[nodiscard]] constexpr TempCategory biome_temp_category(std::int32_t id) noexcept {
    switch (id) {
        case 10: case 11: case 12: case 13: case 26: case 140:
            return TempCategory::cold;
        case 2: case 8: case 17: case 35: case 36: case 37: case 38: case 39:
        case 130: case 163: case 164: case 165: case 166: case 167:
            return TempCategory::warm;
        default:
            return TempCategory::medium;
    }
}

// GenLayerBiomeEdge 的邻居谓词 b(a,b)：群系类等价，或温度类别相同，
// 或任一方是 MEDIUM（MEDIUM 与任何类别都算匹配）。
[[nodiscard]] constexpr bool same_biome_or_temperature(std::int32_t a,
                                                       std::int32_t b) noexcept {
    if (biomes_equal_or_mesa_plateau(a, b)) return true;
    const auto ca = biome_temp_category(a);
    const auto cb = biome_temp_category(b);
    return ca == cb || ca == TempCategory::medium || cb == TempCategory::medium;
}

// Biome.isSnowyBiome = properties.enableSnow，由 setSnowEnabled() 置位。
// 全部 9 个：frozen_ocean/frozen_river/ice_flats/ice_mountains/cold_beach/
// taiga_cold/taiga_cold_hills/mutated_ice_flats/mutated_taiga_cold。
[[nodiscard]] constexpr bool is_snowy_biome(std::int32_t id) noexcept {
    switch (id) {
        case 10: case 11: case 12: case 13: case 26: case 30: case 31:
        case 140: case 158:
            return true;
        default:
            return false;
    }
}

}  // namespace cyane::world::biome_table
