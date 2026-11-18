#pragma once

#include "cyane/world/gen_layer.hpp"

namespace cyane::world::layer {

// GenLayerAddIsland：在已有陆地上长出新的陆地格，并在 1/5 概率下侵蚀海岸。
// 关键点：四个邻格的取用顺序是 左上/右上/左下/右下，
// 且 `k1 != 0 && nextInt(l2++) == 0` 是短路求值——邻格为 0 时既不取随机数
// 也不自增计数，随机序列依赖于此，必须逐字复刻。
class GenLayerAddIsland final : public GenLayer {
public:
    GenLayerAddIsland(std::int64_t base_seed, std::shared_ptr<GenLayer> parent)
        : GenLayer(base_seed) {
        parent_ = std::move(parent);
    }

    [[nodiscard]] std::vector<std::int32_t> get_ints(std::int32_t area_x, std::int32_t area_y,
                                                     std::int32_t width,
                                                     std::int32_t height) override {
        const std::int32_t i = area_x - 1;
        const std::int32_t j = area_y - 1;
        const std::int32_t k = width + 2;
        const std::int32_t l = height + 2;
        const auto src = parent_->get_ints(i, j, k, l);
        std::vector<std::int32_t> out(
            static_cast<std::size_t>(width) * static_cast<std::size_t>(height), 0);

        for (std::int32_t i1 = 0; i1 < height; ++i1) {
            for (std::int32_t j1 = 0; j1 < width; ++j1) {
                const auto at = [&](std::int32_t dx, std::int32_t dy) {
                    return src[static_cast<std::size_t>(j1 + dx) +
                               static_cast<std::size_t>(i1 + dy) * static_cast<std::size_t>(k)];
                };
                const std::int32_t k1 = at(0, 0);
                const std::int32_t l1 = at(2, 0);
                const std::int32_t i2 = at(0, 2);
                const std::int32_t j2 = at(2, 2);
                const std::int32_t k2 = at(1, 1);
                seed_.init_chunk_seed(area_x + j1, area_y + i1);
                auto& dst = out[static_cast<std::size_t>(j1) +
                                static_cast<std::size_t>(i1) * static_cast<std::size_t>(width)];

                if (k2 != 0 || (k1 == 0 && l1 == 0 && i2 == 0 && j2 == 0)) {
                    if (k2 > 0 && (k1 == 0 || l1 == 0 || i2 == 0 || j2 == 0)) {
                        if (seed_.next_int(5) == 0) {
                            dst = k2 == 4 ? 4 : 0;
                        } else {
                            dst = k2;
                        }
                    } else {
                        dst = k2;
                    }
                } else {
                    std::int32_t pick = 1;
                    std::int32_t counter = 1;
                    if (k1 != 0 && seed_.next_int(counter++) == 0) pick = k1;
                    if (l1 != 0 && seed_.next_int(counter++) == 0) pick = l1;
                    if (i2 != 0 && seed_.next_int(counter++) == 0) pick = i2;
                    if (j2 != 0 && seed_.next_int(counter++) == 0) pick = j2;
                    if (seed_.next_int(3) == 0) {
                        dst = pick;
                    } else if (pick == 4) {
                        dst = 4;
                    } else {
                        dst = 0;
                    }
                }
            }
        }
        return out;
    }
};

}  // namespace cyane::world::layer
