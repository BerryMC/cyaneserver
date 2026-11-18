#pragma once

#include "cyane/world/gen_layer.hpp"

namespace cyane::world::layer {

// GenLayerBiome：把温度带值（1..4 + 高位标记）映射到具体群系 id。
// 数据取自 Spigot 反编译（net.minecraft.server.v1_12_R1.GenLayerBiome）：
//   c = {desert,desert,desert,savanna,savanna,plains}        温度带 1
//   d = {forest,roofed_forest,extreme_hills,plains,
//        birch_forest,swampland}                             温度带 2
//   e = {forest,extreme_hills,taiga,plains}                  温度带 3
//   f = {ice_flats,ice_flats,ice_flats,taiga_cold}           温度带 4
// NORMAL_1_1 世界类型会把 c 换成 {desert,forest,extreme_hills,swampland,plains,taiga}。
class GenLayerBiome final : public GenLayer {
public:
    GenLayerBiome(std::int64_t base_seed, std::shared_ptr<GenLayer> parent, bool normal_1_1)
        : GenLayer(base_seed), normal_1_1_(normal_1_1) {
        parent_ = std::move(parent);
    }

    [[nodiscard]] std::vector<std::int32_t> get_ints(std::int32_t area_x, std::int32_t area_y,
                                                     std::int32_t width,
                                                     std::int32_t height) override {
        const auto src = parent_->get_ints(area_x, area_y, width, height);
        std::vector<std::int32_t> out(src.size(), 0);
        for (std::int32_t i = 0; i < height; ++i) {
            for (std::int32_t j = 0; j < width; ++j) {
                seed_.init_chunk_seed(area_x + j, area_y + i);
                const auto index = static_cast<std::size_t>(j) +
                                   static_cast<std::size_t>(i) * static_cast<std::size_t>(width);
                std::int32_t value = src[index];
                const std::int32_t flag = (value & 3840) >> 8;
                value &= -3841;
                out[index] = map_band(value, flag);
            }
        }
        return out;
    }

private:
    [[nodiscard]] std::int32_t map_band(std::int32_t value, std::int32_t flag) {
        if (is_oceanic(value)) {
            return value;
        }
        if (value == biome_ids::kMushroomIsland) {
            return value;
        }
        switch (value) {
            case 1:
                if (flag > 0) {
                    return seed_.next_int(3) == 0 ? biome_ids::kMesaClearRock
                                                  : biome_ids::kMesaRock;
                }
                return pick_desert_band();
            case 2:
                if (flag > 0) {
                    return biome_ids::kJungle;  // Biomes.w
                }
                return pick(warm_band());
            case 3:
                if (flag > 0) {
                    return biome_ids::kRedwoodTaiga;
                }
                return pick(cool_band());
            case 4:
                return pick(icy_band());
            default:
                return biome_ids::kMushroomIsland;
        }
    }

    [[nodiscard]] static bool is_oceanic(std::int32_t id) noexcept {
        return id == biome_ids::kOcean || id == biome_ids::kDeepOcean ||
               id == biome_ids::kFrozenOcean;
    }

    [[nodiscard]] std::int32_t pick(const std::vector<std::int32_t>& options) {
        return options[static_cast<std::size_t>(seed_.next_int(
            static_cast<std::int32_t>(options.size())))];
    }

    [[nodiscard]] std::vector<std::int32_t> desert_band() const {
        if (normal_1_1_) {
            return {biome_ids::kDesert, biome_ids::kForest, biome_ids::kExtremeHills,
                    biome_ids::kSwampland, biome_ids::kPlains, biome_ids::kTaiga};
        }
        return {biome_ids::kDesert, biome_ids::kDesert, biome_ids::kDesert,
                biome_ids::kSavanna, biome_ids::kSavanna, biome_ids::kPlains};
    }

    [[nodiscard]] static std::vector<std::int32_t> warm_band() {
        return {biome_ids::kForest, biome_ids::kRoofedForest, biome_ids::kExtremeHills,
                biome_ids::kPlains, biome_ids::kBirchForest, biome_ids::kSwampland};
    }

    [[nodiscard]] static std::vector<std::int32_t> cool_band() {
        return {biome_ids::kForest, biome_ids::kExtremeHills, biome_ids::kTaiga,
                biome_ids::kPlains};
    }

    [[nodiscard]] static std::vector<std::int32_t> icy_band() {
        return {biome_ids::kIceFlats, biome_ids::kIceFlats, biome_ids::kIceFlats,
                biome_ids::kTaigaCold};
    }

    [[nodiscard]] std::int32_t pick_desert_band() { return pick(desert_band()); }

    bool normal_1_1_{false};
};

}  // namespace cyane::world::layer
