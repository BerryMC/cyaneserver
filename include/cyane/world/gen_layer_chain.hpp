#pragma once

#include "cyane/world/gen_layer_add_island.hpp"
#include "cyane/world/gen_layer_add_snow.hpp"
#include "cyane/world/gen_layer_biome.hpp"
#include "cyane/world/gen_layer_biome_edge.hpp"
#include "cyane/world/gen_layer_edge.hpp"
#include "cyane/world/gen_layer_hills.hpp"
#include "cyane/world/gen_layer_island.hpp"
#include "cyane/world/gen_layer_ocean.hpp"
#include "cyane/world/gen_layer_rare_biome.hpp"
#include "cyane/world/gen_layer_remove_ocean.hpp"
#include "cyane/world/gen_layer_river.hpp"
#include "cyane/world/gen_layer_shore.hpp"
#include "cyane/world/gen_layer_smooth.hpp"
#include "cyane/world/gen_layer_voronoi.hpp"
#include "cyane/world/gen_layer_zoom.hpp"

namespace cyane::world::layer {

// GenLayer.initializeAllBiomeGenerators 的 WorldType.DEFAULT 分支（不含 fixedBiome
// 与 NORMAL_1_1 等变体）。返回最终用于查群系的 Voronoi 层与河流混合层。
struct BiomeChain {
    std::shared_ptr<GenLayer> rivermix;  // 未做 Voronoi 的 1:1 比例层
    std::shared_ptr<GenLayer> voronoi;   // 4x 放大的最终层
    // 按方块坐标取群系 id（内部按 4 格一单元做 Voronoi）
    [[nodiscard]] std::int32_t biome_at(std::int32_t block_x, std::int32_t block_z) const {
        const auto values = voronoi->get_ints(block_x, block_z, 1, 1);
        return values.empty() ? 0 : values[0];
    }
};

[[nodiscard]] inline BiomeChain make_overworld_biome_chain(std::int64_t seed) {
    constexpr std::int32_t kBiomeSize = 4;
    constexpr std::int32_t kRiverSize = 4;

    std::shared_ptr<GenLayer> layer = std::make_shared<GenLayerIsland>(1);
    layer = std::make_shared<GenLayerFuzzyZoom>(2000, layer);
    layer = std::make_shared<GenLayerAddIsland>(1, layer);
    layer = std::make_shared<GenLayerZoom>(2001, layer);
    layer = std::make_shared<GenLayerAddIsland>(2, layer);
    layer = std::make_shared<GenLayerAddIsland>(50, layer);
    layer = std::make_shared<GenLayerAddIsland>(70, layer);
    layer = std::make_shared<GenLayerRemoveTooMuchOcean>(2, layer);
    layer = std::make_shared<GenLayerAddSnow>(2, layer);
    layer = std::make_shared<GenLayerAddIsland>(3, layer);
    layer = std::make_shared<GenLayerEdge>(2, layer, GenLayerEdge::Mode::cool_warm);
    layer = std::make_shared<GenLayerEdge>(2, layer, GenLayerEdge::Mode::heat_ice);
    layer = std::make_shared<GenLayerEdge>(3, layer, GenLayerEdge::Mode::special);
    layer = std::make_shared<GenLayerZoom>(2002, layer);
    layer = std::make_shared<GenLayerZoom>(2003, layer);
    layer = std::make_shared<GenLayerAddIsland>(4, layer);
    layer = std::make_shared<GenLayerAddMushroomIsland>(5, layer);
    std::shared_ptr<GenLayer> base = std::make_shared<GenLayerDeepOcean>(4, layer);

    // 河流链：RiverInit → 两次 Zoom → riverSize 次 Zoom → River
    std::shared_ptr<GenLayer> river_init = std::make_shared<GenLayerRiverInit>(100, base);
    std::shared_ptr<GenLayer> river_zoom = magnify(1000, river_init, 2);
    river_zoom = magnify(1000, river_zoom, kRiverSize);
    auto river = std::make_shared<GenLayerRiver>(1, river_zoom);
    auto river_smooth = std::make_shared<GenLayerSmooth>(1000, river);

    // 群系链：Biome → BiomeEdge → Hills（第二父层是 RiverInit 的 2 次 Zoom）
    auto biome = std::make_shared<GenLayerBiome>(200, base, false);
    auto biome_edge = std::make_shared<GenLayerBiomeEdge>(1000, biome);
    auto hills_source = magnify(1000, river_init, 2);
    std::shared_ptr<GenLayer> hills = std::make_shared<GenLayerHills>(1000, biome_edge, hills_source);
    hills = std::make_shared<GenLayerRareBiome>(1001, hills);

    for (std::int32_t k = 0; k < kBiomeSize; ++k) {
        hills = std::make_shared<GenLayerZoom>(1000 + k, hills);
        if (k == 0) {
            hills = std::make_shared<GenLayerAddIsland>(3, hills);
        }
        if (k == 1 || kBiomeSize == 1) {
            hills = std::make_shared<GenLayerShore>(1000, hills);
        }
    }

    auto hills_smooth = std::make_shared<GenLayerSmooth>(1000, hills);
    auto rivermix = std::make_shared<GenLayerRiverMix>(100, hills_smooth, river_smooth);
    auto voronoi = std::make_shared<GenLayerVoronoiZoom>(10, rivermix);

    rivermix->init_world_gen_seed(seed);
    voronoi->init_world_gen_seed(seed);
    return BiomeChain{rivermix, voronoi};
}

}  // namespace cyane::world::layer
