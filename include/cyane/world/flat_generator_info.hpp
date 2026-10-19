#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include "cyane/world/blocks.hpp"

namespace cyane::world::gen {

// 1.12.2 超平坦单层描述（FlatLayerInfo 逐行转写）
class FlatLayerInfo {
public:
    constexpr FlatLayerInfo(int layer_count, std::uint16_t block_state) noexcept
        : layer_count_(layer_count), block_state_(block_state), min_y_(0) {}

    constexpr FlatLayerInfo(int version, int layer_count, std::uint16_t block_state) noexcept
        : layer_count_(layer_count), block_state_(block_state), min_y_(0) {
        (void)version;
    }

    [[nodiscard]] constexpr int get_layer_count() const noexcept { return layer_count_; }
    [[nodiscard]] constexpr std::uint16_t get_block_state() const noexcept { return block_state_; }
    [[nodiscard]] constexpr int get_min_y() const noexcept { return min_y_; }
    constexpr void set_min_y(int min_y) noexcept { min_y_ = min_y; }

private:
    int layer_count_;
    std::uint16_t block_state_;
    int min_y_;
};

// 1.12.2 超平坦预设解析器（FlatGeneratorInfo 逐行转写）
// 格式："3;minecraft:bedrock,2*minecraft:dirt,minecraft:grass;1;village"
class FlatGeneratorInfo {
public:
    FlatGeneratorInfo() = default;

    [[nodiscard]] int get_biome() const noexcept { return biome_id_; }
    void set_biome(int biome_id) noexcept { biome_id_ = biome_id; }

    [[nodiscard]] const std::vector<FlatLayerInfo>& get_flat_layers() const noexcept {
        return flat_layers_;
    }

    void add_layer(const FlatLayerInfo& layer) {
        flat_layers_.push_back(layer);
        update_layer_heights();
    }

    void update_layer_heights() noexcept {
        int y = 0;
        for (auto& layer : flat_layers_) {
            layer.set_min_y(y);
            y += layer.get_layer_count();
        }
    }

    [[nodiscard]] static FlatGeneratorInfo create_flat_preset(std::string_view preset) {
        FlatGeneratorInfo info;
        if (preset.empty()) {
            return get_default_flat_generator();
        }

        // 解析版本、方块层与生物群系: "3;minecraft:bedrock,2*minecraft:dirt,minecraft:grass;1"
        const auto p1 = preset.find(';');
        if (p1 == std::string_view::npos) return get_default_flat_generator();

        const auto p2 = preset.find(';', p1 + 1);
        std::string_view layers_str = (p2 == std::string_view::npos) ? preset.substr(p1 + 1) : preset.substr(p1 + 1, p2 - p1 - 1);

        // 解析图层
        std::size_t start = 0;
        while (start < layers_str.size()) {
            const auto comma = layers_str.find(',', start);
            const auto layer_token = (comma == std::string_view::npos) ? layers_str.substr(start) : layers_str.substr(start, comma - start);

            int count = 1;
            std::string_view block_name = layer_token;
            const auto star = layer_token.find('*');
            if (star != std::string_view::npos) {
                count = std::stoi(std::string(layer_token.substr(0, star)));
                block_name = layer_token.substr(star + 1);
            }

            std::uint16_t state = kStateDirt;
            if (block_name == "minecraft:bedrock" || block_name == "7") state = kStateBedrock;
            else if (block_name == "minecraft:dirt" || block_name == "3") state = kStateDirt;
            else if (block_name == "minecraft:grass" || block_name == "2") state = kStateGrass;
            else if (block_name == "minecraft:sand" || block_name == "12") state = kStateSand;
            else if (block_name == "minecraft:stone" || block_name == "1") state = kStateStone;

            info.add_layer(FlatLayerInfo{count, state});

            if (comma == std::string_view::npos) break;
            start = comma + 1;
        }

        if (p2 != std::string_view::npos) {
            const auto p3 = preset.find(';', p2 + 1);
            const auto biome_token = (p3 == std::string_view::npos) ? preset.substr(p2 + 1) : preset.substr(p2 + 1, p3 - p2 - 1);
            if (!biome_token.empty()) {
                try {
                    info.set_biome(std::stoi(std::string(biome_token)));
                } catch (...) {
                    info.set_biome(1);
                }
            }
        }

        return info;
    }

    [[nodiscard]] static FlatGeneratorInfo get_default_flat_generator() {
        FlatGeneratorInfo info;
        info.set_biome(1); // Plains
        info.add_layer(FlatLayerInfo{1, kStateBedrock});
        info.add_layer(FlatLayerInfo{2, kStateDirt});
        info.add_layer(FlatLayerInfo{1, kStateGrass});
        return info;
    }

private:
    int biome_id_{1};
    std::vector<FlatLayerInfo> flat_layers_;
};

}  // namespace cyane::world::gen
