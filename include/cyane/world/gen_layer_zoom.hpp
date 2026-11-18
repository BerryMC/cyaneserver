#pragma once

#include "cyane/world/gen_layer.hpp"

namespace cyane::world::layer {

// GenLayerZoom：把父层放大 2 倍。每个 2x2 输出块由父层 2x2 邻域决定：
//   左上 = 原值；右上 = rand(左, 下)；左下 = rand(左, 右)；右下 = modeOrRandom(四个)
class GenLayerZoom : public GenLayer {
public:
    GenLayerZoom(std::int64_t base_seed, std::shared_ptr<GenLayer> parent)
        : GenLayer(base_seed), parent_owner_(std::move(parent)) {
        parent_ = parent_owner_;
    }

    [[nodiscard]] std::vector<std::int32_t> get_ints(std::int32_t area_x, std::int32_t area_y,
                                                     std::int32_t width,
                                                     std::int32_t height) override {
        const std::int32_t i = area_x >> 1;
        const std::int32_t j = area_y >> 1;
        const std::int32_t k = (width >> 1) + 2;
        const std::int32_t l = (height >> 1) + 2;
        const auto src = parent_->get_ints(i, j, k, l);

        const std::int32_t i1 = (k - 1) << 1;
        const std::int32_t j1 = (l - 1) << 1;
        std::vector<std::int32_t> zoomed(
            static_cast<std::size_t>(i1) * static_cast<std::size_t>(j1), 0);

        for (std::int32_t k1 = 0; k1 < l - 1; ++k1) {
            std::int32_t l1 = (k1 << 1) * i1;
            std::int32_t i2 = 0;
            std::int32_t j2 = src[static_cast<std::size_t>(i2) + static_cast<std::size_t>(k1) * static_cast<std::size_t>(k)];
            std::int32_t k2 = src[static_cast<std::size_t>(i2) +
                                  static_cast<std::size_t>(k1 + 1) * static_cast<std::size_t>(k)];
            for (; i2 < k - 1; ++i2) {
                seed_.init_chunk_seed(static_cast<std::int64_t>(i2 + i) << 1,
                                      static_cast<std::int64_t>(k1 + j) << 1);
                const std::int32_t l2 = src[static_cast<std::size_t>(i2 + 1) +
                                            static_cast<std::size_t>(k1) * static_cast<std::size_t>(k)];
                const std::int32_t i3 = src[static_cast<std::size_t>(i2 + 1) +
                                            static_cast<std::size_t>(k1 + 1) * static_cast<std::size_t>(k)];
                zoomed[static_cast<std::size_t>(l1)] = j2;
                zoomed[static_cast<std::size_t>(l1 + i1)] = select_random({j2, k2});
                ++l1;
                zoomed[static_cast<std::size_t>(l1)] = select_random({j2, l2});
                zoomed[static_cast<std::size_t>(l1 + i1)] =
                    select_mode_or_random(j2, l2, k2, i3);
                ++l1;
                j2 = l2;
                k2 = i3;
            }
        }

        std::vector<std::int32_t> out(
            static_cast<std::size_t>(width) * static_cast<std::size_t>(height), 0);
        for (std::int32_t j3 = 0; j3 < height; ++j3) {
            const auto from = static_cast<std::size_t>(
                static_cast<std::int64_t>(j3 + (area_y & 1)) * i1 + (area_x & 1));
            for (std::int32_t x = 0; x < width; ++x) {
                out[static_cast<std::size_t>(j3) * static_cast<std::size_t>(width) +
                    static_cast<std::size_t>(x)] =
                    zoomed[from + static_cast<std::size_t>(x)];
            }
        }
        return out;
    }

private:
    std::shared_ptr<GenLayer> parent_owner_;
};

// GenLayerFuzzyZoom：把 modeOrRandom 换成纯随机
class GenLayerFuzzyZoom final : public GenLayerZoom {
public:
    GenLayerFuzzyZoom(std::int64_t base_seed, std::shared_ptr<GenLayer> parent)
        : GenLayerZoom(base_seed, std::move(parent)) {}

protected:
    [[nodiscard]] std::int32_t select_mode_or_random(std::int32_t a, std::int32_t b,
                                                     std::int32_t c, std::int32_t d) override {
        return select_random({a, b, c, d});
    }
};

// GenLayerZoom.magnify：连续套 times 层 Zoom，种子为 seed + i
[[nodiscard]] inline std::shared_ptr<GenLayer> magnify(std::int64_t seed,
                                                       std::shared_ptr<GenLayer> layer,
                                                       std::int32_t times) {
    for (std::int32_t i = 0; i < times; ++i) {
        layer = std::make_shared<GenLayerZoom>(seed + i, std::move(layer));
    }
    return layer;
}

}  // namespace cyane::world::layer
