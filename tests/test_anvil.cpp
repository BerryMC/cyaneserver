#include <algorithm>
#include <filesystem>

#include "cyane/game/world_persistence.hpp"
#include "cyane/net/furnace_store.hpp"
#include "cyane/net/item_drop.hpp"
#include "cyane/world/anvil.hpp"
#include "cyane/world/blocks.hpp"
#include "cyane/world/nbt.hpp"
#include "cyane/world/region.hpp"
#include "test_framework.hpp"

using namespace cyane;
using world::ChunkPos;

namespace {
constexpr std::int16_t kApple = 260;  // 物品 id

} // namespace

CYANE_TEST(anvil_chunk_round_trip_with_entities) {
    const ChunkPos pos{3, -2};
    // 方块：石头 / 点亮熔炉（meta 点亮位）/ 箱子 / 基线同值方块（无损语义下同样编码）
    world::Chunk chunk{pos};
    chunk.set_block_state(pos.world_x() + 0, 4, pos.world_z() + 5, world::kStateStone);
    chunk.set_block_state(pos.world_x() + 15, 9, pos.world_z() + 15,
                          static_cast<std::uint16_t>(world::kStateFurnace | 8));
    chunk.set_block_state(pos.world_x() + 2, 4, pos.world_z() + 2, world::kStateChest);
    chunk.set_block_state(pos.world_x() + 3, 3, pos.world_z() + 3, world::kStateGrass);

    world::ChunkEntities entities;
    world::StoredChest chest{};
    chest[0] = item::ItemStack{kApple, 5, 0};
    chest[26] = item::ItemStack{std::int16_t{1}, 1, 3};
    entities.chests.emplace_back(world::pack_block_pos(pos.world_x() + 2, 4, pos.world_z() + 2),
                                 chest);
    world::StoredFurnace furnace;
    furnace.input = item::ItemStack{15, 3, 0};   // 铁矿
    furnace.output = item::ItemStack{265, 1, 0}; // 铁锭
    furnace.burn_left = 1200;
    furnace.burn_total = 1600;
    furnace.cook_time = 55;
    entities.furnaces.emplace_back(world::pack_block_pos(pos.world_x() + 15, 9, pos.world_z() + 15),
                                   furnace);

    auto encoded = world::encode_chunk(pos, chunk, entities);
    CYANE_CHECK(encoded.has_value());
    auto decoded = world::decode_chunk(ByteSpan{*encoded});
    CYANE_CHECK(decoded.has_value());

    // 方块逐一还原（无损：chunk 级往返）
    const auto check_state = [&](std::int32_t wx, std::int32_t wy, std::int32_t wz,
                                 std::uint16_t expected) {
        const auto got = decoded->chunk.block_state(wx, wy, wz);
        CYANE_CHECK(got.has_value());
        CYANE_CHECK_EQ(*got, expected);
    };
    check_state(pos.world_x() + 0, 4, pos.world_z() + 5, world::kStateStone);
    check_state(pos.world_x() + 15, 9, pos.world_z() + 15,
                static_cast<std::uint16_t>(world::kStateFurnace | 8));
    check_state(pos.world_x() + 2, 4, pos.world_z() + 2, world::kStateChest);
    check_state(pos.world_x() + 3, 3, pos.world_z() + 3, world::kStateGrass);
    // 未写的位置为空气
    check_state(pos.world_x() + 8, 4, pos.world_z() + 8, world::kStateAir);
    CYANE_CHECK_EQ(decoded->chunk.pos(), pos);

    CYANE_CHECK_EQ(decoded->entities.chests.size(), std::size_t{1});
    CYANE_CHECK_EQ(decoded->entities.chests[0].first,
                   world::pack_block_pos(pos.world_x() + 2, 4, pos.world_z() + 2));
    CYANE_CHECK_EQ(decoded->entities.chests[0].second[0].id, kApple);
    CYANE_CHECK_EQ(decoded->entities.chests[0].second[0].count, std::uint8_t{5});
    CYANE_CHECK_EQ(decoded->entities.chests[0].second[26].damage, std::int16_t{3});
    CYANE_CHECK(decoded->entities.chests[0].second[1].empty());

    CYANE_CHECK_EQ(decoded->entities.furnaces.size(), std::size_t{1});
    const auto& restored = decoded->entities.furnaces[0].second;
    CYANE_CHECK_EQ(restored.input.id, std::int16_t{15});
    CYANE_CHECK_EQ(restored.input.count, std::uint8_t{3});
    CYANE_CHECK_EQ(restored.output.id, std::int16_t{265});
    CYANE_CHECK_EQ(restored.burn_left, 1200);
    CYANE_CHECK_EQ(restored.burn_total, 1600);
    CYANE_CHECK_EQ(restored.cook_time, 55);

    // 掉落物：写入 Entities，解码还原
    entities.items.push_back(world::StoredEntity{1.25, 3.5, -2.75, item::ItemStack{kApple, 9, 0}});
    entities.items.push_back(world::StoredEntity{5.0, 5.0, 5.0,
                                                 item::ItemStack{std::int16_t{1}, 1, 3}});
    encoded = world::encode_chunk(pos, chunk, entities);
    CYANE_CHECK(encoded.has_value());
    decoded = world::decode_chunk(ByteSpan{*encoded});
    CYANE_CHECK(decoded.has_value());
    CYANE_CHECK_EQ(decoded->entities.items.size(), std::size_t{2});
    CYANE_CHECK(decoded->entities.items[0].x == 1.25 && decoded->entities.items[0].z == -2.75);
    CYANE_CHECK_EQ(decoded->entities.items[0].stack.id, kApple);
    CYANE_CHECK_EQ(decoded->entities.items[0].stack.count, std::uint8_t{9});
    CYANE_CHECK_EQ(decoded->entities.items[1].stack.damage, std::int16_t{3});
}

CYANE_TEST(anvil_all_air_chunk_encodes_with_empty_sections) {
    const ChunkPos pos{0, 0};
    world::Chunk chunk{pos};
    auto encoded = world::encode_chunk(pos, chunk, world::ChunkEntities{});
    CYANE_CHECK(encoded.has_value());
    auto decoded = world::decode_chunk(ByteSpan{*encoded});
    CYANE_CHECK(decoded.has_value());
    CYANE_CHECK(decoded->chunk.sections().empty());
    CYANE_CHECK(decoded->entities.empty());
}

CYANE_TEST(region_write_read_round_trip) {
    const auto dir = std::filesystem::temp_directory_path() / "cyane_test_region";
    std::filesystem::remove_all(dir);
    const auto path = dir / "r.0.0.mca";

    world::RegionFile file;
    const Bytes small(64, std::byte{0x11});
    const Bytes big(70000, std::byte{0x22});  // 跨多扇区
    auto w1 = file.write_chunk(0, 0, ByteSpan{small});
    auto w2 = file.write_chunk(31, 31, ByteSpan{big});
    auto w3 = file.write_chunk(-1, 0, ByteSpan{small});  // 负坐标折算到 in-region (31,0)
    CYANE_CHECK(w1.has_value() && w2.has_value() && w3.has_value());
    CYANE_CHECK(file.dirty());

    CYANE_CHECK(file.save(path).has_value());

    auto reloaded = world::RegionFile::load(path);
    CYANE_CHECK(reloaded.has_value());
    auto r1 = reloaded->read_chunk(0, 0);
    CYANE_CHECK(r1.has_value() && r1->has_value());
    CYANE_CHECK_EQ((*r1)->size(), small.size());
    auto r2 = reloaded->read_chunk(31, 31);
    CYANE_CHECK(r2.has_value() && r2->has_value());
    CYANE_CHECK_EQ((*r2)->size(), big.size());
    auto r3 = reloaded->read_chunk(31, 0);
    CYANE_CHECK(r3.has_value() && r3->has_value());
    CYANE_CHECK_EQ((*r3)->size(), small.size());
    // 未写区块不存在
    auto r4 = reloaded->read_chunk(5, 5);
    CYANE_CHECK(r4.has_value() && !r4->has_value());

    // 覆写为更大载荷后再读
    const Bytes bigger(120000, std::byte{0x33});
    CYANE_CHECK(reloaded->write_chunk(0, 0, ByteSpan{bigger}).has_value());
    CYANE_CHECK(reloaded->save(path).has_value());
    auto again = world::RegionFile::load(path);
    CYANE_CHECK(again.has_value());
    auto r5 = again->read_chunk(0, 0);
    CYANE_CHECK(r5.has_value() && r5->has_value());
    CYANE_CHECK_EQ((*r5)->size(), bigger.size());

    std::filesystem::remove_all(dir);
}

CYANE_TEST(persistence_world_round_trip) {
    const auto dir = std::filesystem::temp_directory_path() / "cyane_test_world";
    std::filesystem::remove_all(dir);

    world::World source_world;
    net::ContainerStore source_chests;
    net::FurnaceStore source_furnaces;
    source_furnaces.set_tables({}, {});

    // 编辑两个区块：石头平台 + 箱子 + 熔炉（跨 region：区块 0,0 与 -1,-1）
    source_world.set_block(1, 4, 1, world::kStateStone);
    source_world.set_block(2, 5, 2, world::kStateChest);
    source_world.set_block(-1, 6, -1, world::kStateFurnace);
    const std::int64_t chest_key = world::pack_block_pos(2, 5, 2);
    source_chests.ensure(chest_key);
    source_chests.set_slot(chest_key, 3, item::ItemStack{kApple, 12, 0});
    const std::int64_t furnace_key = world::pack_block_pos(-1, 6, -1);
    source_furnaces.ensure(furnace_key);
    source_furnaces.restore(furnace_key, [&] {
        net::FurnaceState state;
        state.input = item::ItemStack{15, 2, 0};
        state.cook_time = 100;
        state.burn_total = 1600;
        return state;
    }());
    net::ItemDropManager source_drops;
    source_drops.spawn(1.5, 5.5, 1.5, item::ItemStack{kApple, 7, 0}, 0);

    {
        game::WorldPersistence saver(source_world, source_chests, source_furnaces, source_drops,
                                    dir.string());
        auto saved = saver.save();
        CYANE_CHECK(saved.has_value());
        CYANE_CHECK_EQ(*saved, std::size_t{2});
        CYANE_CHECK(std::filesystem::exists(dir / "region" / "r.0.0.mca"));
        CYANE_CHECK(std::filesystem::exists(dir / "region" / "r.-1.-1.mca"));
    }

    // 全新内存态重新载入
    world::World loaded_world;
    net::ContainerStore loaded_chests;
    net::FurnaceStore loaded_furnaces;
    net::ItemDropManager loaded_drops;
    loaded_furnaces.set_tables({}, {});
    game::WorldPersistence loader(loaded_world, loaded_chests, loaded_furnaces, loaded_drops,
                                  dir.string());
    auto loaded = loader.load();
    CYANE_CHECK(loaded.has_value());
    CYANE_CHECK_EQ(*loaded, std::size_t{2});

    CYANE_CHECK_EQ(loaded_world.block_at(1, 4, 1), world::kStateStone);
    CYANE_CHECK_EQ(loaded_world.block_at(2, 5, 2), world::kStateChest);
    CYANE_CHECK_EQ(loaded_world.block_at(-1, 6, -1), world::kStateFurnace);
    CYANE_CHECK_EQ(loaded_world.block_at(1, 3, 1), world::flat_baseline(3));
    // 未编辑区块仍是超平坦
    CYANE_CHECK_EQ(loaded_world.block_at(500, 0, 500), world::kStateBedrock);
    CYANE_CHECK_EQ(loaded_world.block_at(500, 3, 500), world::kStateGrass);

    const auto chest = loaded_chests.snapshot(chest_key);
    CYANE_CHECK_EQ(chest[3].id, kApple);
    CYANE_CHECK_EQ(chest[3].count, std::uint8_t{12});
    const auto furnace = loaded_furnaces.snapshot(furnace_key);
    CYANE_CHECK_EQ(furnace.input.id, std::int16_t{15});
    CYANE_CHECK_EQ(furnace.cook_time, 100);

    // 掉落物实体往返
    const auto restored_drops = loaded_drops.all_drops();
    CYANE_CHECK_EQ(restored_drops.size(), std::size_t{1});
    CYANE_CHECK_EQ(restored_drops[0].stack.id, kApple);
    CYANE_CHECK_EQ(restored_drops[0].stack.count, std::uint8_t{7});
    CYANE_CHECK(restored_drops[0].x == 1.5 && restored_drops[0].z == 1.5);

    // 再次保存：方块区块全部干净（启动载入不标脏）不重写；
    // 实体区块（箱子/熔炉所在）无条件重写——容器内容变更没有脏标记，保守起见
    auto resaved = loader.save();
    CYANE_CHECK(resaved.has_value());
    CYANE_CHECK_EQ(*resaved, std::size_t{2});

    std::filesystem::remove_all(dir);
}
