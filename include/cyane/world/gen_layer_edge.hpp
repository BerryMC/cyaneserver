#pragma once

#include "cyane/world/gen_layer.hpp"

namespace cyane::world::layer {

// GenLayerEdge：在冷暖/干湿分界上插入过渡值（2 = 温带，3 = 寒带）。
// 三个模式的父层区域取法不同，这里逐字保留：
//   COOL_WARM / HEAT_ICE —— 父区域扩张 1 格，取四邻
//   SPECIAL              —— 父区域不扩张，只按 1/13 概率给陆地块打上高位标记
class GenLayerEdge final : public GenLayer {
public:
    enum class Mode { cool_warm, heat_ice, special };

    GenLayerEdge(std::int64_t base_seed, std::shared_ptr<GenLayer> parent, Mode mode)
        : GenLayer(base_seed), mode_(mode) {
        parent_ = std::move(parent);
    }

    [[nodiscard]] std::vector<std::int32_t> get_ints(std::int32_t area_x, std::int32_t area_y,
                                                     std::int32_t width,
                                                     std::int32_t height) override {
        switch (mode_) {
            case Mode::heat_ice:
                return heat_ice(area_x, area_y, width, height);
            case Mode::special:
                return special(area_x, area_y, width, height);
            case Mode::cool_warm:
            default:
                return cool_warm(area_x, area_y, width, height);
        }
    }

private:
    [[nodiscard]] std::vector<std::int32_t> cool_warm(std::int32_t area_x, std::int32_t area_y,
                                                      std::int32_t width, std::int32_t height) {
        const std::int32_t k = width + 2;
        const auto src = parent_->get_ints(area_x - 1, area_y - 1, k, height + 2);
        std::vector<std::int32_t> out(
            static_cast<std::size_t>(width) * static_cast<std::size_t>(height), 0);
        for (std::int32_t i1 = 0; i1 < height; ++i1) {
            for (std::int32_t j1 = 0; j1 < width; ++j1) {
                seed_.init_chunk_seed(area_x + j1, area_y + i1);
                const auto at = [&](std::int32_t dx, std::int32_t dy) {
                    return src[static_cast<std::size_t>(j1 + 1 + dx) +
                               static_cast<std::size_t>(i1 + 1 + dy) *
                                   static_cast<std::size_t>(k)];
                };
                std::int32_t value = at(0, 0);
                if (value == 1) {
                    const std::int32_t up = at(0, -1);
                    const std::int32_t right = at(1, 0);
                    const std::int32_t left = at(-1, 0);
                    const std::int32_t down = at(0, 1);
                    const bool has_warm = up == 3 || right == 3 || left == 3 || down == 3;
                    const bool has_hot = up == 4 || right == 4 || left == 4 || down == 4;
                    if (has_warm || has_hot) {
                        value = 2;
                    }
                }
                out[static_cast<std::size_t>(j1) +
                    static_cast<std::size_t>(i1) * static_cast<std::size_t>(width)] = value;
            }
        }
        return out;
    }

    // 原版此分支不调用 initChunkSeed，不消耗随机数
    [[nodiscard]] std::vector<std::int32_t> heat_ice(std::int32_t area_x, std::int32_t area_y,
                                                     std::int32_t width, std::int32_t height) {
        const std::int32_t k = width + 2;
        const auto src = parent_->get_ints(area_x - 1, area_y - 1, k, height + 2);
        std::vector<std::int32_t> out(
            static_cast<std::size_t>(width) * static_cast<std::size_t>(height), 0);
        for (std::int32_t i1 = 0; i1 < height; ++i1) {
            for (std::int32_t j1 = 0; j1 < width; ++j1) {
                const auto at = [&](std::int32_t dx, std::int32_t dy) {
                    return src[static_cast<std::size_t>(j1 + 1 + dx) +
                               static_cast<std::size_t>(i1 + 1 + dy) *
                                   static_cast<std::size_t>(k)];
                };
                std::int32_t value = at(0, 0);
                if (value == 4) {
                    const std::int32_t up = at(0, -1);
                    const std::int32_t right = at(1, 0);
                    const std::int32_t left = at(-1, 0);
                    const std::int32_t down = at(0, 1);
                    const bool has_temperate = up == 2 || right == 2 || left == 2 || down == 2;
                    const bool has_cold = up == 1 || right == 1 || left == 1 || down == 1;
                    if (has_cold || has_temperate) {
                        value = 3;
                    }
                }
                out[static_cast<std::size_t>(j1) +
                    static_cast<std::size_t>(i1) * static_cast<std::size_t>(width)] = value;
            }
        }
        return out;
    }

    [[nodiscard]] std::vector<std::int32_t> special(std::int32_t area_x, std::int32_t area_y,
                                                    std::int32_t width, std::int32_t height) {
        const auto src = parent_->get_ints(area_x, area_y, width, height);
        std::vector<std::int32_t> out(
            static_cast<std::size_t>(width) * static_cast<std::size_t>(height), 0);
        for (std::int32_t i = 0; i < height; ++i) {
            for (std::int32_t j = 0; j < width; ++j) {
                seed_.init_chunk_seed(area_x + j, area_y + i);
                std::int32_t value = src[static_cast<std::size_t>(j) +
                                         static_cast<std::size_t>(i) *
                                             static_cast<std::size_t>(width)];
                // Java: k |= 1 + nextInt(15) << 8 & 3840
                // 优先级：+ 高于 <<，<< 高于 & → ((1 + n) << 8) & 0xF00
                if (value != 0 && seed_.next_int(13) == 0) {
                    value |= ((1 + seed_.next_int(15)) << 8) & 3840;
                }
                out[static_cast<std::size_t>(j) +
                    static_cast<std::size_t>(i) * static_cast<std::size_t>(width)] = value;
            }
        }
        return out;
    }

    Mode mode_;
};

}  // namespace cyane::world::layer
