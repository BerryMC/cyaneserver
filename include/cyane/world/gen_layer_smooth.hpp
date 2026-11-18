#pragma once

#include "cyane/world/gen_layer.hpp"

namespace cyane::world::layer {

// GenLayerSmooth：抹平锯齿。左右相同取左右，上下相同取上下；
// 只有"两者都相同"的分支才消耗一次随机数。
class GenLayerSmooth final : public GenLayer {
public:
    GenLayerSmooth(std::int64_t base_seed, std::shared_ptr<GenLayer> parent)
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
                const std::int32_t left = at(0, 1);
                const std::int32_t right = at(2, 1);
                const std::int32_t up = at(1, 0);
                const std::int32_t down = at(1, 2);
                std::int32_t center = at(1, 1);
                if (left == right && up == down) {
                    seed_.init_chunk_seed(area_x + j1, area_y + i1);
                    center = seed_.next_int(2) == 0 ? left : up;
                } else {
                    if (left == right) center = left;
                    if (up == down) center = up;
                }
                out[static_cast<std::size_t>(j1) +
                    static_cast<std::size_t>(i1) * static_cast<std::size_t>(width)] = center;
            }
        }
        return out;
    }
};

}  // namespace cyane::world::layer
