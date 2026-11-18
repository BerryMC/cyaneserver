#pragma once

#include "cyane/world/gen_layer.hpp"

namespace cyane::world::layer {

// GenLayerIsland：以 1/10 概率生成陆地(1)，其余为海洋(0)；
// 原点所在的方块强制为陆地（保证出生点不在海里）。
class GenLayerIsland final : public GenLayer {
public:
    explicit GenLayerIsland(std::int64_t base_seed) : GenLayer(base_seed) {}

    [[nodiscard]] std::vector<std::int32_t> get_ints(std::int32_t area_x, std::int32_t area_y,
                                                     std::int32_t width,
                                                     std::int32_t height) override {
        std::vector<std::int32_t> out(
            static_cast<std::size_t>(width) * static_cast<std::size_t>(height), 0);
        for (std::int32_t i = 0; i < height; ++i) {
            for (std::int32_t j = 0; j < width; ++j) {
                seed_.init_chunk_seed(area_x + j, area_y + i);
                out[static_cast<std::size_t>(j) +
                    static_cast<std::size_t>(i) * static_cast<std::size_t>(width)] =
                    seed_.next_int(10) == 0 ? 1 : 0;
            }
        }
        // 原点强制陆地：areaX > -width && areaX <= 0 && areaY > -height && areaY <= 0
        if (area_x > -width && area_x <= 0 && area_y > -height && area_y <= 0) {
            out[static_cast<std::size_t>(-area_x) +
                static_cast<std::size_t>(-area_y) * static_cast<std::size_t>(width)] = 1;
        }
        return out;
    }
};

}  // namespace cyane::world::layer
