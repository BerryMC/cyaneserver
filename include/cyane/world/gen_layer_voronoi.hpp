#pragma once

#include <cmath>

#include "cyane/world/gen_layer.hpp"

namespace cyane::world::layer {

// GenLayerVoronoiZoom：按 4 格一个"细胞"做 Voronoi 划分，让群系边界变成曲线。
// 原版有几个必须照抄的细节：
//   · 入参先把 areaX/areaY 各减 2，父层按 >>2 取
//   · 每个细胞用 (nextInt(1024)/1024 - 0.5) * 3.6 抖动，邻细胞 +4
//   · 距离里 i3 与 d2 配对、k3 与 d1 配对（横纵是交叉的）
//   · 父层取值 & 255
class GenLayerVoronoiZoom final : public GenLayer {
public:
    GenLayerVoronoiZoom(std::int64_t base_seed, std::shared_ptr<GenLayer> parent)
        : GenLayer(base_seed) {
        parent_ = std::move(parent);
    }

    [[nodiscard]] std::vector<std::int32_t> get_ints(std::int32_t area_x, std::int32_t area_y,
                                                     std::int32_t width,
                                                     std::int32_t height) override {
        area_x -= 2;
        area_y -= 2;
        const std::int32_t i = area_x >> 2;
        const std::int32_t j = area_y >> 2;
        const std::int32_t k = (width >> 2) + 2;
        const std::int32_t l = (height >> 2) + 2;
        const auto src = parent_->get_ints(i, j, k, l);
        const std::int32_t i1 = (k - 1) << 2;
        const std::int32_t j1 = (l - 1) << 2;
        std::vector<std::int32_t> cells(
            static_cast<std::size_t>(i1) * static_cast<std::size_t>(j1), 0);

        const auto noise = [this]() {
            return (static_cast<double>(seed_.next_int(1024)) / 1024.0 - 0.5) * 3.6;
        };

        for (std::int32_t k1 = 0; k1 < l - 1; ++k1) {
            std::int32_t l1 = 0;
            std::int32_t i2 = src[static_cast<std::size_t>(l1) +
                                  static_cast<std::size_t>(k1) * static_cast<std::size_t>(k)];
            std::int32_t j2 = src[static_cast<std::size_t>(l1) +
                                  static_cast<std::size_t>(k1 + 1) * static_cast<std::size_t>(k)];
            while (l1 < k - 1) {
                seed_.init_chunk_seed(static_cast<std::int64_t>(l1 + i) << 2,
                                      static_cast<std::int64_t>(k1 + j) << 2);
                const double d1 = noise();
                const double d2 = noise();
                seed_.init_chunk_seed(static_cast<std::int64_t>(l1 + i + 1) << 2,
                                      static_cast<std::int64_t>(k1 + j) << 2);
                const double d3 = noise() + 4.0;
                const double d4 = noise();
                seed_.init_chunk_seed(static_cast<std::int64_t>(l1 + i) << 2,
                                      static_cast<std::int64_t>(k1 + j + 1) << 2);
                const double d5 = noise();
                const double d6 = noise() + 4.0;
                seed_.init_chunk_seed(static_cast<std::int64_t>(l1 + i + 1) << 2,
                                      static_cast<std::int64_t>(k1 + j + 1) << 2);
                const double d7 = noise() + 4.0;
                const double d8 = noise() + 4.0;

                const std::int32_t k2 =
                    src[static_cast<std::size_t>(l1 + 1) +
                        static_cast<std::size_t>(k1) * static_cast<std::size_t>(k)] &
                    255;
                const std::int32_t l2 =
                    src[static_cast<std::size_t>(l1 + 1) +
                        static_cast<std::size_t>(k1 + 1) * static_cast<std::size_t>(k)] &
                    255;

                for (std::int32_t i3 = 0; i3 < 4; ++i3) {
                    auto j3 = static_cast<std::size_t>((k1 << 2) + i3) *
                                  static_cast<std::size_t>(i1) +
                              static_cast<std::size_t>(l1 << 2);
                    for (std::int32_t k3 = 0; k3 < 4; ++k3) {
                        const double x = static_cast<double>(i3);
                        const double y = static_cast<double>(k3);
                        const double d9 = (x - d2) * (x - d2) + (y - d1) * (y - d1);
                        const double d10 = (x - d4) * (x - d4) + (y - d3) * (y - d3);
                        const double d11 = (x - d6) * (x - d6) + (y - d5) * (y - d5);
                        const double d12 = (x - d8) * (x - d8) + (y - d7) * (y - d7);
                        std::int32_t pick = 0;
                        if (d9 < d10 && d9 < d11 && d9 < d12) {
                            pick = i2;
                        } else if (d10 < d9 && d10 < d11 && d10 < d12) {
                            pick = k2;
                        } else if (d11 < d9 && d11 < d10 && d11 < d12) {
                            pick = j2;
                        } else {
                            pick = l2;
                        }
                        cells[j3++] = pick;
                    }
                }
                i2 = k2;
                j2 = l2;
                ++l1;
            }
        }

        std::vector<std::int32_t> out(
            static_cast<std::size_t>(width) * static_cast<std::size_t>(height), 0);
        for (std::int32_t l3 = 0; l3 < height; ++l3) {
            const auto from = static_cast<std::size_t>(
                static_cast<std::int64_t>(l3 + (area_y & 3)) * i1 + (area_x & 3));
            for (std::int32_t x = 0; x < width; ++x) {
                out[static_cast<std::size_t>(l3) * static_cast<std::size_t>(width) +
                    static_cast<std::size_t>(x)] = cells[from + static_cast<std::size_t>(x)];
            }
        }
        return out;
    }
};

}  // namespace cyane::world::layer
