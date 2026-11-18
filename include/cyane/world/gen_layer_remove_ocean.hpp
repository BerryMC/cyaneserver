#pragma once

#include "cyane/world/gen_layer.hpp"

namespace cyane::world::layer {

// GenLayerRemoveTooMuchOcean：四面都是海的中心格以 1/2 概率变成陆地，
// 打散大片纯海洋。原版邻居索引里写的是 (areaWidth + 2) 而非 k，
// 两者相等，这里统一用 k。
class GenLayerRemoveTooMuchOcean final : public GenLayer {
public:
    GenLayerRemoveTooMuchOcean(std::int64_t base_seed, std::shared_ptr<GenLayer> parent)
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
                const std::int32_t up = at(1, 0);
                const std::int32_t right = at(2, 1);
                const std::int32_t left = at(0, 1);
                const std::int32_t down = at(1, 2);
                std::int32_t center = at(1, 1);
                seed_.init_chunk_seed(area_x + j1, area_y + i1);
                if (center == 0 && up == 0 && right == 0 && left == 0 && down == 0 &&
                    seed_.next_int(2) == 0) {
                    center = 1;
                }
                out[static_cast<std::size_t>(j1) +
                    static_cast<std::size_t>(i1) * static_cast<std::size_t>(width)] = center;
            }
        }
        return out;
    }
};

}  // namespace cyane::world::layer
