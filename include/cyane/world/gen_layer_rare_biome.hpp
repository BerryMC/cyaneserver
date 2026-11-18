#pragma once

#include "cyane/world/gen_layer.hpp"

namespace cyane::world::layer {

// GenLayerRareBiome：1/57 概率把 plains 换成 mutated_plains（向日葵平原）。
// 数据取自 Spigot 反编译（v1_12_R1.GenLayerPlains）：
//   只有 k == plains 才替换，其余值原样返回——**不会**做 k + 128，
//   这点与一些二手资料里的写法不同。
class GenLayerRareBiome final : public GenLayer {
public:
    GenLayerRareBiome(std::int64_t base_seed, std::shared_ptr<GenLayer> parent)
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
        for (std::int32_t i = 0; i < height; ++i) {
            for (std::int32_t j = 0; j < width; ++j) {
                seed_.init_chunk_seed(area_x + j, area_y + i);
                std::int32_t value = src[static_cast<std::size_t>(j + 1) +
                                         static_cast<std::size_t>(i + 1) *
                                             static_cast<std::size_t>(k)];
                if (seed_.next_int(57) == 0 && value == biome_ids::kPlains) {
                    value = biome_ids::kMutatedPlains;
                }
                out[static_cast<std::size_t>(j) +
                    static_cast<std::size_t>(i) * static_cast<std::size_t>(width)] = value;
            }
        }
        return out;
    }
};

}  // namespace cyane::world::layer
