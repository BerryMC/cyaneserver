#pragma once

#include "cyane/world/gen_layer.hpp"

namespace cyane::world::layer {

// GenLayerAddSnow：把陆地值细分为温度带。
// 0 → 海洋(0)；其余按 nextInt(6)：0→4、<=1→3、否则→1。
class GenLayerAddSnow final : public GenLayer {
public:
    GenLayerAddSnow(std::int64_t base_seed, std::shared_ptr<GenLayer> parent)
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
                const std::int32_t value =
                    src[static_cast<std::size_t>(j1 + 1) +
                        static_cast<std::size_t>(i1 + 1) * static_cast<std::size_t>(k)];
                seed_.init_chunk_seed(area_x + j1, area_y + i1);
                std::int32_t result = 0;
                if (value != 0) {
                    const std::int32_t roll = seed_.next_int(6);
                    if (roll == 0) {
                        result = 4;
                    } else if (roll <= 1) {
                        result = 3;
                    } else {
                        result = 1;
                    }
                }
                out[static_cast<std::size_t>(j1) +
                    static_cast<std::size_t>(i1) * static_cast<std::size_t>(width)] = result;
            }
        }
        return out;
    }
};

}  // namespace cyane::world::layer
