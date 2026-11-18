#pragma once

#include "cyane/world/gen_layer.hpp"

namespace cyane::world::layer {

// Biomes.MUSHROOM_ISLAND / Biomes.DEEP_OCEAN 的注册 id
inline constexpr std::int32_t kBiomeMushroomIsland = biome_ids::kMushroomIsland;
inline constexpr std::int32_t kBiomeDeepOcean = biome_ids::kDeepOcean;

// GenLayerAddMushroomIsland：四面皆海的中心格以 1/100 概率变成蘑菇岛
class GenLayerAddMushroomIsland final : public GenLayer {
public:
    GenLayerAddMushroomIsland(std::int64_t base_seed, std::shared_ptr<GenLayer> parent)
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
                    return src[static_cast<std::size_t>(j1 + dx) +
                               static_cast<std::size_t>(i1 + dy) * static_cast<std::size_t>(k)];
                };
                const std::int32_t center = at(1, 1);
                seed_.init_chunk_seed(area_x + j1, area_y + i1);
                std::int32_t value = center;
                if (center == 0 && at(0, 0) == 0 && at(2, 0) == 0 && at(0, 2) == 0 &&
                    at(2, 2) == 0 && seed_.next_int(100) == 0) {
                    value = kBiomeMushroomIsland;
                }
                out[static_cast<std::size_t>(j1) +
                    static_cast<std::size_t>(i1) * static_cast<std::size_t>(width)] = value;
            }
        }
        return out;
    }
};

// GenLayerDeepOcean：四面中 3 个以上为海的中心海格变成深海
class GenLayerDeepOcean final : public GenLayer {
public:
    GenLayerDeepOcean(std::int64_t base_seed, std::shared_ptr<GenLayer> parent)
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
                    return src[static_cast<std::size_t>(j1 + dx) +
                               static_cast<std::size_t>(i1 + dy) * static_cast<std::size_t>(k)];
                };
                const std::int32_t center = at(1, 1);
                int ocean_neighbours = 0;
                if (at(1, 0) == 0) ++ocean_neighbours;
                if (at(2, 1) == 0) ++ocean_neighbours;
                if (at(0, 1) == 0) ++ocean_neighbours;
                if (at(1, 2) == 0) ++ocean_neighbours;
                out[static_cast<std::size_t>(j1) +
                    static_cast<std::size_t>(i1) * static_cast<std::size_t>(width)] =
                    (center == 0 && ocean_neighbours > 3) ? kBiomeDeepOcean : center;
            }
        }
        return out;
    }
};

}  // namespace cyane::world::layer
