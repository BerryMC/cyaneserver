#pragma once

#include "cyane/world/biome_table.hpp"
#include "cyane/world/gen_layer.hpp"

namespace cyane::world::layer {

// GenLayerShore：把海岸线切成沙滩/石岸/冷岸，并处理蘑菇岛岸与丛林边缘。
// 逐行对应 Spigot 反编译的 v1_12_R1.GenLayerMushroomShore。
class GenLayerShore final : public GenLayer {
public:
    GenLayerShore(std::int64_t base_seed, std::shared_ptr<GenLayer> parent)
        : GenLayer(base_seed) {
        parent_ = std::move(parent);
    }

    [[nodiscard]] std::vector<std::int32_t> get_ints(std::int32_t area_x, std::int32_t area_y,
                                                     std::int32_t width,
                                                     std::int32_t height) override {
        const std::int32_t k = width + 2;
        const auto src = parent_->get_ints(area_x - 1, area_y - 1, k, height + 2);
        std::vector<std::int32_t> out(
            static_cast<std::size_t>(width) * static_cast<std::size_t>(height), 0);

        const auto at = [&](std::int32_t j, std::int32_t i, std::int32_t dx, std::int32_t dy) {
            return src[static_cast<std::size_t>(j + 1 + dx) +
                       static_cast<std::size_t>(i + 1 + dy) * static_cast<std::size_t>(k)];
        };

        for (std::int32_t i = 0; i < height; ++i) {
            for (std::int32_t j = 0; j < width; ++j) {
                seed_.init_chunk_seed(area_x + j, area_y + i);
                const std::int32_t biome = at(j, i, 0, 0);
                const auto up = at(j, i, 0, -1);
                const auto right = at(j, i, 1, 0);
                const auto left = at(j, i, -1, 0);
                const auto down = at(j, i, 0, 1);
                const bool any_ocean = is_oceanic(up) || is_oceanic(right) ||
                                       is_oceanic(left) || is_oceanic(down);
                const bool all_ocean = is_oceanic(up) && is_oceanic(right) &&
                                       is_oceanic(left) && is_oceanic(down);
                auto& dst = out[static_cast<std::size_t>(j) +
                                static_cast<std::size_t>(i) * static_cast<std::size_t>(width)];

                if (biome == biome_ids::kMushroomIsland) {
                    // 四周只要有海洋就退成蘑菇岛岸
                    dst = any_ocean ? biome_ids::kMushroomIslandShore
                                    : biome_ids::kMushroomIsland;
                    continue;
                }
                if (is_jungle_class(biome)) {
                    if (!is_jungle_like(up) || !is_jungle_like(right) ||
                        !is_jungle_like(left) || !is_jungle_like(down)) {
                        dst = biome_ids::kJungleEdge;
                    } else {
                        dst = all_ocean ? 16 : biome;  // 16 = beaches
                    }
                    continue;
                }
                if (biome == biome_ids::kExtremeHills ||
                    biome == biome_ids::kExtremeHillsWithTrees ||
                    biome == biome_ids::kSmallerExtremeHills) {
                    dst = replace(biome, 25, any_ocean);  // 25 = stone_beach
                    continue;
                }
                if (biome_table::is_snowy_biome(biome)) {
                    dst = replace(biome, 26, any_ocean);  // 26 = cold_beach
                    continue;
                }
                if (biome == biome_ids::kMesa || biome == biome_ids::kMesaRock) {
                    if (any_ocean) {
                        dst = biome;
                    } else if (is_mesa_class(up) && is_mesa_class(right) &&
                               is_mesa_class(left) && is_mesa_class(down)) {
                        dst = biome;
                    } else {
                        dst = biome_ids::kDesert;
                    }
                    continue;
                }
                if (biome != biome_ids::kOcean && biome != biome_ids::kDeepOcean &&
                    biome != biome_ids::kRiver && biome != biome_ids::kSwampland) {
                    dst = any_ocean ? 16 : biome;
                    continue;
                }
                dst = biome;
            }
        }
        return out;
    }

private:
    [[nodiscard]] static bool is_oceanic(std::int32_t id) noexcept {
        return biome_table::biome_class(id) == biome_table::BiomeClass::ocean;
    }
    [[nodiscard]] static bool is_jungle_class(std::int32_t id) noexcept {
        return biome_table::biome_class(id) == biome_table::BiomeClass::jungle;
    }
    [[nodiscard]] static bool is_mesa_class(std::int32_t id) noexcept {
        return biome_table::biome_class(id) == biome_table::BiomeClass::mesa;
    }
    // GenLayerShore.c(int)：丛林类，或 {jungle_edge,jungle,jungle_hills,forest,taiga}，或海洋
    [[nodiscard]] static bool is_jungle_like(std::int32_t id) noexcept {
        if (is_jungle_class(id)) return true;
        if (id == biome_ids::kJungleEdge || id == biome_ids::kJungle ||
            id == biome_ids::kJungleHills || id == biome_ids::kForest ||
            id == biome_ids::kTaiga) {
            return true;
        }
        return is_oceanic(id);
    }
    // GenLayerShore.replaceBiomeEdge：海洋本身保持；四邻无海洋则保持；否则换成 replacement
    [[nodiscard]] static std::int32_t replace(std::int32_t biome, std::int32_t replacement,
                                              bool any_ocean) noexcept {
        if (is_oceanic(biome)) return biome;
        return any_ocean ? replacement : biome;
    }
};

}  // namespace cyane::world::layer
