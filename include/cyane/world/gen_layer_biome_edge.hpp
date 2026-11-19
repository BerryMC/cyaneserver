#pragma once

#include "cyane/world/biome_table.hpp"
#include "cyane/world/gen_layer.hpp"

namespace cyane::world::layer {

// GenLayerBiomeEdge：把极端群系的边界替换成更温和的近邻，避免突兀接缝。
// 逐行对应 Spigot 反编译的 v1_12_R1.GenLayerDesert。
//
// 四组前置替换按顺序短路，注意两套谓词不同：
//   第 1 组 replaceBiomeEdgeIfNecessary —— 外层用群系类等价，邻居用"温度感知"谓词
//   第 2..4 组 replaceBiomeEdge        —— 外层用 ==，邻居用群系类等价
class GenLayerBiomeEdge final : public GenLayer {
public:
    GenLayerBiomeEdge(std::int64_t base_seed, std::shared_ptr<GenLayer> parent)
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
                auto& dst = out[static_cast<std::size_t>(j) +
                                static_cast<std::size_t>(i) * static_cast<std::size_t>(width)];
                const auto up = at(j, i, 0, -1);
                const auto right = at(j, i, 1, 0);
                const auto left = at(j, i, -1, 0);
                const auto down = at(j, i, 0, 1);

                // 1) extreme_hills → 四邻仍属 extreme_hills 家族则留 hills_with_trees，否则降为 smaller
                if (biome_table::biomes_equal_or_mesa_plateau(biome, biome_ids::kExtremeHills)) {
                    const bool all_near =
                        biome_table::same_biome_or_temperature(up, biome_ids::kExtremeHills) &&
                        biome_table::same_biome_or_temperature(right, biome_ids::kExtremeHills) &&
                        biome_table::same_biome_or_temperature(left, biome_ids::kExtremeHills) &&
                        biome_table::same_biome_or_temperature(down, biome_ids::kExtremeHills);
                    dst = all_near ? biome_ids::kExtremeHillsWithTrees
                                   : biome_ids::kSmallerExtremeHills;
                    continue;
                }
                // 2..4) 台地与红木林的边缘
                if (biome == biome_ids::kMesaRock) {
                    const bool all_near =
                        biome_table::biomes_equal_or_mesa_plateau(up, biome_ids::kMesaRock) &&
                        biome_table::biomes_equal_or_mesa_plateau(right, biome_ids::kMesaRock) &&
                        biome_table::biomes_equal_or_mesa_plateau(left, biome_ids::kMesaRock) &&
                        biome_table::biomes_equal_or_mesa_plateau(down, biome_ids::kMesaRock);
                    dst = all_near ? biome_ids::kMesaRock : biome_ids::kMesa;
                    continue;
                }
                if (biome == biome_ids::kMesaClearRock) {
                    const bool all_near =
                        biome_table::biomes_equal_or_mesa_plateau(up, biome_ids::kMesaClearRock) &&
                        biome_table::biomes_equal_or_mesa_plateau(right, biome_ids::kMesaClearRock) &&
                        biome_table::biomes_equal_or_mesa_plateau(left, biome_ids::kMesaClearRock) &&
                        biome_table::biomes_equal_or_mesa_plateau(down, biome_ids::kMesaClearRock);
                    dst = all_near ? biome_ids::kMesaClearRock : biome_ids::kMesa;
                    continue;
                }
                if (biome == biome_ids::kRedwoodTaiga) {
                    const bool all_near =
                        biome_table::biomes_equal_or_mesa_plateau(up, biome_ids::kRedwoodTaiga) &&
                        biome_table::biomes_equal_or_mesa_plateau(right, biome_ids::kRedwoodTaiga) &&
                        biome_table::biomes_equal_or_mesa_plateau(left, biome_ids::kRedwoodTaiga) &&
                        biome_table::biomes_equal_or_mesa_plateau(down, biome_ids::kRedwoodTaiga);
                    dst = all_near ? biome_ids::kRedwoodTaiga : biome_ids::kTaiga;
                    continue;
                }
                if (biome == biome_ids::kDesert) {
                    if (up == biome_ids::kIceFlats || right == biome_ids::kIceFlats ||
                        left == biome_ids::kIceFlats || down == biome_ids::kIceFlats) {
                        dst = biome_ids::kExtremeHillsWithTrees;
                    } else {
                        dst = biome_ids::kDesert;
                    }
                    continue;
                }
                if (biome == biome_ids::kSwampland) {
                    const bool cold_near =
                        up == biome_ids::kDesert || right == biome_ids::kDesert ||
                        left == biome_ids::kDesert || down == biome_ids::kDesert ||
                        up == biome_ids::kTaigaCold || right == biome_ids::kTaigaCold ||
                        left == biome_ids::kTaigaCold || down == biome_ids::kTaigaCold ||
                        up == biome_ids::kIceFlats || right == biome_ids::kIceFlats ||
                        left == biome_ids::kIceFlats || down == biome_ids::kIceFlats;
                    if (cold_near) {
                        dst = biome_ids::kPlains;
                    } else if (up == biome_ids::kJungle || right == biome_ids::kJungle ||
                               left == biome_ids::kJungle || down == biome_ids::kJungle) {
                        dst = biome_ids::kJungleEdge;
                    } else {
                        dst = biome_ids::kSwampland;
                    }
                    continue;
                }
                dst = biome;
            }
        }
        return out;
    }
};

}  // namespace cyane::world::layer
