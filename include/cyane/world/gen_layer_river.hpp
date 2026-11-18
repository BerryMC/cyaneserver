#pragma once

#include "cyane/world/gen_layer.hpp"

namespace cyane::world::layer {

// GenLayerRiverInit：陆地上分配河流种子值 nextInt(299999) + 2，海洋保持 0。
// 父区域不扩张。
class GenLayerRiverInit final : public GenLayer {
public:
    GenLayerRiverInit(std::int64_t base_seed, std::shared_ptr<GenLayer> parent)
        : GenLayer(base_seed) {
        parent_ = std::move(parent);
    }

    [[nodiscard]] std::vector<std::int32_t> get_ints(std::int32_t area_x, std::int32_t area_y,
                                                     std::int32_t width,
                                                     std::int32_t height) override {
        const auto src = parent_->get_ints(area_x, area_y, width, height);
        std::vector<std::int32_t> out(src.size(), 0);
        for (std::int32_t i = 0; i < height; ++i) {
            for (std::int32_t j = 0; j < width; ++j) {
                seed_.init_chunk_seed(area_x + j, area_y + i);
                const auto index = static_cast<std::size_t>(j) +
                                   static_cast<std::size_t>(i) * static_cast<std::size_t>(width);
                out[index] = src[index] > 0 ? seed_.next_int(299999) + 2 : 0;
            }
        }
        return out;
    }
};

// GenLayerRiver：四邻与中心经 riverFilter 后全部相同 → -1（无河），否则为河流 id
class GenLayerRiver final : public GenLayer {
public:
    GenLayerRiver(std::int64_t base_seed, std::shared_ptr<GenLayer> parent)
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
        for (std::int32_t i1 = 0; i1 < height; ++i1) {
            for (std::int32_t j1 = 0; j1 < width; ++j1) {
                const auto at = [&](std::int32_t dx, std::int32_t dy) {
                    return filter(src[static_cast<std::size_t>(j1 + dx) +
                                      static_cast<std::size_t>(i1 + dy) *
                                          static_cast<std::size_t>(k)]);
                };
                const std::int32_t left = at(0, 1);
                const std::int32_t right = at(2, 1);
                const std::int32_t up = at(1, 0);
                const std::int32_t down = at(1, 2);
                const std::int32_t center = at(1, 1);
                out[static_cast<std::size_t>(j1) +
                    static_cast<std::size_t>(i1) * static_cast<std::size_t>(width)] =
                    (center == left && center == up && center == right && center == down)
                        ? -1
                        : biome_ids::kRiver;
            }
        }
        return out;
    }

private:
    [[nodiscard]] static constexpr std::int32_t filter(std::int32_t value) noexcept {
        return value >= 2 ? 2 + (value & 1) : value;
    }
};

// GenLayerRiverMix：把河流层叠回群系层。海洋不变；冰原上的河 → 冻河；
// 蘑菇岛/蘑菇岛岸上的河 → 蘑菇岛岸；其余取河流层低 8 位。
class GenLayerRiverMix final : public GenLayer {
public:
    GenLayerRiverMix(std::int64_t base_seed, std::shared_ptr<GenLayer> biome_chain,
                     std::shared_ptr<GenLayer> river_chain)
        : GenLayer(base_seed),
          biome_chain_(std::move(biome_chain)),
          river_chain_(std::move(river_chain)) {
        parent_ = biome_chain_;
    }

    void init_world_gen_seed(std::int64_t seed) override {
        biome_chain_->init_world_gen_seed(seed);
        river_chain_->init_world_gen_seed(seed);
        seed_.init_world_gen_seed(seed);
    }

    [[nodiscard]] std::vector<std::int32_t> get_ints(std::int32_t area_x, std::int32_t area_y,
                                                     std::int32_t width,
                                                     std::int32_t height) override {
        const auto biomes = biome_chain_->get_ints(area_x, area_y, width, height);
        const auto rivers = river_chain_->get_ints(area_x, area_y, width, height);
        std::vector<std::int32_t> out(biomes.size(), 0);
        for (std::size_t i = 0; i < out.size(); ++i) {
            const auto biome = biomes[i];
            if (biome == biome_ids::kOcean || biome == biome_ids::kDeepOcean) {
                out[i] = biome;
            } else if (rivers[i] != biome_ids::kRiver) {
                out[i] = biome;
            } else if (biome == biome_ids::kIcePlains) {
                out[i] = biome_ids::kFrozenRiver;
            } else if (biome == biome_ids::kMushroomIsland ||
                       biome == biome_ids::kMushroomIslandShore) {
                out[i] = biome_ids::kMushroomIslandShore;
            } else {
                out[i] = rivers[i] & 255;
            }
        }
        return out;
    }

private:
    std::shared_ptr<GenLayer> biome_chain_;
    std::shared_ptr<GenLayer> river_chain_;
};

}  // namespace cyane::world::layer
