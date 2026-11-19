#pragma once

#include "cyane/world/biome_table.hpp"
#include "cyane/world/gen_layer.hpp"

namespace cyane::world::layer {

// GenLayerHills：在两父层之上把部分群系换成丘陵/变体。
// 第二个父层是河流初始层（RiverInit），其值与 29 取模决定"变体标记"。
// 逻辑逐行对应 Spigot 反编译的 v1_12_R1.GenLayerRegionHills。
class GenLayerHills final : public GenLayer {
public:
    GenLayerHills(std::int64_t base_seed, std::shared_ptr<GenLayer> biome_parent,
                  std::shared_ptr<GenLayer> river_parent)
        : GenLayer(base_seed),
          biome_parent_(std::move(biome_parent)),
          river_parent_(std::move(river_parent)) {
        parent_ = biome_parent_;
    }

    void init_world_gen_seed(std::int64_t seed) override {
        biome_parent_->init_world_gen_seed(seed);
        river_parent_->init_world_gen_seed(seed);
        seed_.init_world_gen_seed(seed);
    }

    [[nodiscard]] std::vector<std::int32_t> get_ints(std::int32_t area_x, std::int32_t area_y,
                                                     std::int32_t width,
                                                     std::int32_t height) override {
        const std::int32_t k = width + 2;
        const auto biomes = biome_parent_->get_ints(area_x - 1, area_y - 1, k, height + 2);
        const auto rivers = river_parent_->get_ints(area_x - 1, area_y - 1, k, height + 2);
        std::vector<std::int32_t> out(
            static_cast<std::size_t>(width) * static_cast<std::size_t>(height), 0);

        const auto at = [&](const std::vector<std::int32_t>& src, std::int32_t dx,
                            std::int32_t dy, std::int32_t j, std::int32_t i) {
            return src[static_cast<std::size_t>(j + dx) +
                       static_cast<std::size_t>(i + dy) * static_cast<std::size_t>(k)];
        };

        for (std::int32_t i = 0; i < height; ++i) {
            for (std::int32_t j = 0; j < width; ++j) {
                seed_.init_chunk_seed(area_x + j, area_y + i);
                const std::int32_t biome = at(biomes, 1, 1, j, i);
                const std::int32_t river = at(rivers, 1, 1, j, i);
                const bool variant_flag = (river - 2) % 29 == 0;
                const bool already_mutated = biome_table::is_mutation(biome);
                auto& dst = out[static_cast<std::size_t>(j) +
                                static_cast<std::size_t>(i) * static_cast<std::size_t>(width)];

                // 河种值 ≡ 1 (mod 29) 且当前不是变体 → 直接升为变体
                if (biome != 0 && river >= 2 && (river - 2) % 29 == 1 && !already_mutated) {
                    const auto mutation = biome_table::mutation_of(biome);
                    dst = mutation.value_or(biome);
                    continue;
                }
                if (seed_.next_int(3) != 0 && !variant_flag) {
                    dst = biome;
                    continue;
                }
                std::int32_t target = biome;
                if (biome == biome_ids::kDesert) {
                    target = biome_ids::kDesertHills;
                } else if (biome == biome_ids::kForest) {
                    target = biome_ids::kForestHills;
                } else if (biome == biome_ids::kBirchForest) {
                    target = biome_ids::kBirchForestHills;
                } else if (biome == biome_ids::kRoofedForest) {
                    target = biome_ids::kPlains;
                } else if (biome == biome_ids::kTaiga) {
                    target = biome_ids::kTaigaHills;
                } else if (biome == biome_ids::kRedwoodTaiga) {
                    target = biome_ids::kRedwoodTaigaHills;
                } else if (biome == biome_ids::kTaigaCold) {
                    target = biome_ids::kTaigaColdHills;
                } else if (biome == biome_ids::kPlains) {
                    target = seed_.next_int(3) == 0 ? biome_ids::kForestHills
                                                    : biome_ids::kForest;
                } else if (biome == biome_ids::kIceFlats) {
                    target = biome_ids::kIceMountains;
                } else if (biome == biome_ids::kJungle) {
                    target = biome_ids::kJungleHills;
                } else if (biome == biome_ids::kOcean) {
                    target = biome_ids::kDeepOcean;
                } else if (biome == biome_ids::kExtremeHills) {
                    target = biome_ids::kExtremeHillsWithTrees;
                } else if (biome == biome_ids::kSavanna) {
                    target = biome_ids::kSavannaRock;
                } else if (biome_table::biomes_equal_or_mesa_plateau(biome,
                                                                    biome_ids::kMesaRock)) {
                    target = biome_ids::kMesa;
                } else if (biome == biome_ids::kDeepOcean && seed_.next_int(3) == 0) {
                    target = seed_.next_int(2) == 0 ? biome_ids::kPlains : biome_ids::kForest;
                }

                std::int32_t chosen = target;
                if (variant_flag && chosen != biome) {
                    const auto mutation = biome_table::mutation_of(chosen);
                    chosen = mutation.value_or(biome);
                }
                if (chosen == biome) {
                    dst = biome;
                    continue;
                }
                // 四邻里至少 3 个与自身同类才允许隆起
                int same = 0;
                if (biome_table::biomes_equal_or_mesa_plateau(at(biomes, 1, 0, j, i), biome)) ++same;
                if (biome_table::biomes_equal_or_mesa_plateau(at(biomes, 2, 1, j, i), biome)) ++same;
                if (biome_table::biomes_equal_or_mesa_plateau(at(biomes, 0, 1, j, i), biome)) ++same;
                if (biome_table::biomes_equal_or_mesa_plateau(at(biomes, 1, 2, j, i), biome)) ++same;
                dst = same >= 3 ? chosen : biome;
            }
        }
        return out;
    }

private:
    std::shared_ptr<GenLayer> biome_parent_;
    std::shared_ptr<GenLayer> river_parent_;
};

}  // namespace cyane::world::layer
