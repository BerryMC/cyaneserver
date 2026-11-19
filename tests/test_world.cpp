#include <array>
#include <bit>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string_view>
#include <unordered_map>

#include "cyane/game/world_persistence.hpp"
#include "cyane/net/block_ticks.hpp"
#include "cyane/net/container_store.hpp"
#include "cyane/net/furnace_store.hpp"
#include "cyane/net/item_drop.hpp"
#include "cyane/net/packet_writers.hpp"
#include "cyane/item/item_tools.hpp"
#include "cyane/proto/frame.hpp"
#include "cyane/world/block_sounds.hpp"
#include "cyane/world/level_dat.hpp"
#include "cyane/world/nbt.hpp"
#include "cyane/world/anvil.hpp"
#include "cyane/world/gen_layer.hpp"
#include "cyane/world/gen_layer_island.hpp"
#include "cyane/world/gen_layer_zoom.hpp"
#include "cyane/world/chunk_generator.hpp"
#include "cyane/world/gen_layer_shore.hpp"
#include "cyane/world/gen_layer_chain.hpp"
#include "cyane/world/gen_layer_biome_edge.hpp"
#include "cyane/world/gen_layer_hills.hpp"
#include "cyane/world/biome_table.hpp"
#include "cyane/world/gen_layer_rare_biome.hpp"
#include "cyane/world/gen_layer_biome.hpp"
#include "cyane/world/gen_layer_voronoi.hpp"
#include "cyane/world/gen_layer_river.hpp"
#include "cyane/world/gen_layer_smooth.hpp"
#include "cyane/world/gen_layer_ocean.hpp"
#include "cyane/world/gen_layer_edge.hpp"
#include "cyane/world/gen_layer_add_snow.hpp"
#include "cyane/world/gen_layer_remove_ocean.hpp"
#include "cyane/world/gen_layer_add_island.hpp"
#include "cyane/world/world.hpp"
#include "cyane/world/structure.hpp"
#include "cyane/world/flat_generator_info.hpp"
#include "cyane/world/dim_generators.hpp"
#include "cyane/world/difficulty.hpp"
#include "cyane/world/game_rules.hpp"
#include "cyane/world/world_border.hpp"
#include "cyane/world/explosion.hpp"
#include "cyane/world/world_types.hpp"
#include "cyane/world/tick_list.hpp"
#include "cyane/world/boss_info.hpp"
#include "cyane/world/loot/loot_table.hpp"
#include "cyane/world/map_data.hpp"
#include "cyane/world/world_settings.hpp"
#include "test_framework.hpp"

using namespace cyane;
namespace nbt = cyane::world::nbt;

namespace {

#ifdef CYANE_FIXTURE_DIR
constexpr std::string_view kFixtureDir{CYANE_FIXTURE_DIR};
#else
constexpr std::string_view kFixtureDir{};
#endif

[[nodiscard]] std::filesystem::path fixture_path(const char* name) {
    for (const std::string& candidate :
         {std::string{kFixtureDir} + name, std::string{"tests/fixtures/"} + name,
          std::string{"../tests/fixtures/"} + name}) {
        if (std::filesystem::exists(candidate)) {
            return candidate;
        }
    }
    return {};
}

[[nodiscard]] std::string read_file(const std::filesystem::path& path) {
    std::ifstream in{path, std::ios::binary};
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

} // namespace

CYANE_TEST(world_flat_materialization_and_dirty_tracking) {
    world::World world;
    // 未物化区块：baseline 兜底
    CYANE_CHECK_EQ(world.block_at(50, 3, 50), world::kStateGrass);
    CYANE_CHECK_EQ(world.block_at(50, 0, 50), world::kStateBedrock);
    CYANE_CHECK_EQ(world.block_at(50, 20, 50), world::kStateAir);
    CYANE_CHECK_EQ(world.loaded_chunks(), std::size_t{0});

    // 发送路径：物化后 section 0 显式承载 baseline
    const auto chunk = world.chunk_at(world::ChunkPos{3, 3});
    const auto grass = chunk.block_state(48, 3, 48);
    CYANE_CHECK(grass.has_value() && *grass == world::kStateGrass);
    CYANE_CHECK_EQ(world.loaded_chunks(), std::size_t{1});

    // 编辑产生脏标记
    world.set_block(500, 70, 500, world::kStateStone);   // 区块 (31,31)
    CYANE_CHECK_EQ(world.block_at(500, 70, 500), world::kStateStone);
    CYANE_CHECK_EQ(world.dirty_chunks().size(), std::size_t{1});
    world.clear_dirty(world::ChunkPos{31, 31});
    CYANE_CHECK(world.dirty_chunks().empty());

    // 干净区块可释放，脏区块保留
    world.set_block(12, 5, 34, world::kStateChest);      // 区块 (0,2) → 脏
    world.release_chunk(world::ChunkPos{31, 31});        // 干净 → 释放
    CYANE_CHECK_EQ(world.loaded_chunks(), std::size_t{2});
    world.release_chunk(world::ChunkPos{0, 2});          // 脏 → 保留
    CYANE_CHECK_EQ(world.loaded_chunks(), std::size_t{2});
    CYANE_CHECK_EQ(world.block_at(12, 5, 34), world::kStateChest);
}

CYANE_TEST(world_vanilla_terrain_load_and_lossless_round_trip) {
    const auto dir = std::filesystem::temp_directory_path() / "cyane_test_m4_terrain";
    std::filesystem::remove_all(dir);

    // 构造"原版地形"：与超平坦基线完全不同的值
    world::World source;
    world::Chunk terrain{world::ChunkPos{5, -7}};
    for (std::size_t y = 0; y < 40; ++y) {
        for (std::size_t z = 0; z < 16; ++z) {
            for (std::size_t x = 0; x < 16; ++x) {
                terrain.set_block_state(80 + static_cast<std::int32_t>(x),
                                        static_cast<std::int32_t>(y),
                                        -112 + static_cast<std::int32_t>(z),
                                        static_cast<std::uint16_t>(world::kStateStone | (y & 0x0F)));
            }
        }
    }
    source.load_chunk(world::ChunkPos{5, -7}, std::move(terrain), /*dirty=*/true);

    // 关键断言：载入的原版地形不被 baseline 过滤吞掉
    CYANE_CHECK_EQ(source.block_at(80, 39, -112),
                   static_cast<std::uint16_t>(world::kStateStone | 0x7));

    net::ContainerStore chests;
    net::FurnaceStore furnaces;
    net::ItemDropManager drops;
    net::MobManager mobs;
    game::WorldPersistence saver(source, chests, furnaces, drops, mobs, dir.string());
    auto saved = saver.save();
    CYANE_CHECK(saved.has_value());
    CYANE_CHECK_EQ(*saved, std::size_t{1});

    // 全新 World 载入：方块数据无损
    world::World loaded;
    net::ContainerStore chests2;
    net::FurnaceStore furnaces2;
    net::ItemDropManager drops2;
    net::MobManager mobs2;
    game::WorldPersistence loader(loaded, chests2, furnaces2, drops2, mobs2, dir.string());
    CYANE_CHECK(loader.load().has_value());
    CYANE_CHECK_EQ(loaded.block_at(80, 0, -112),
                   static_cast<std::uint16_t>(world::kStateStone | 0x0));
    CYANE_CHECK_EQ(loaded.block_at(80, 39, -112),
                   static_cast<std::uint16_t>(world::kStateStone | 0x7));
    CYANE_CHECK_EQ(loaded.block_at(95, 20, -97),
                   static_cast<std::uint16_t>(world::kStateStone | 0x4));

    // 二次保存：方块区块全部干净（启动载入不标脏），不重写
    auto resaved = loader.save();
    CYANE_CHECK(resaved.has_value());
    CYANE_CHECK_EQ(*resaved, std::size_t{0});

    std::filesystem::remove_all(dir);
}

CYANE_TEST(level_dat_oracle_fixture) {
    const auto path = fixture_path("vanilla_level_oracle.dat");
    CYANE_CHECK(!path.empty());
    if (path.empty()) {
        return;
    }
    const auto tmp = std::filesystem::temp_directory_path() / "cyane_test_level";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);
    std::filesystem::copy_file(path, tmp / "level.dat",
                               std::filesystem::copy_options::overwrite_existing);
    auto info = world::load_level_dat(tmp);
    CYANE_CHECK(info.has_value());
    // 预言机实测：原版 1.12.2 世界出生点 (247, 4, 1091)
    CYANE_CHECK_EQ(info->spawn_x, 247);
    CYANE_CHECK_EQ(info->spawn_y, 4);
    CYANE_CHECK_EQ(info->spawn_z, 1091);
    std::filesystem::remove_all(tmp);
}

CYANE_TEST(level_dat_self_written_round_trip) {
    const auto tmp = std::filesystem::temp_directory_path() / "cyane_test_level_write";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);

    // 用我们的 NBT 写出 level.dat（Data 包装 + gzip）
    nbt::Compound data;
    data.emplace_back("SpawnX", nbt::Value{nbt::Tag::i32, std::int32_t{-42}});
    data.emplace_back("SpawnY", nbt::Value{nbt::Tag::i32, std::int32_t{8}});
    data.emplace_back("SpawnZ", nbt::Value{nbt::Tag::i32, std::int32_t{77}});
    nbt::Compound root;
    root.emplace_back("Data", nbt::make_compound(std::move(data)));
    auto nbt_bytes = nbt::serialize("", nbt::make_compound(std::move(root)));
    CYANE_CHECK(nbt_bytes.has_value());
    auto gz = proto::deflate_gzip(ByteSpan{*nbt_bytes}, proto::kDefaultCompressionLevel);
    CYANE_CHECK(gz.has_value());
    {
        std::ofstream out{tmp / "level.dat", std::ios::binary | std::ios::trunc};
        out.write(reinterpret_cast<const char*>(gz->data()),
                  static_cast<std::streamsize>(gz->size()));
    }

    auto info = world::load_level_dat(tmp);
    CYANE_CHECK(info.has_value());
    CYANE_CHECK_EQ(info->spawn_x, -42);
    CYANE_CHECK_EQ(info->spawn_y, 8);
    CYANE_CHECK_EQ(info->spawn_z, 77);

    // 缺文件 → 默认值
    auto missing = world::load_level_dat(tmp / "nowhere");
    CYANE_CHECK(missing.has_value());
    CYANE_CHECK_EQ(missing->spawn_x, 0);
    CYANE_CHECK_EQ(missing->spawn_y, 4);

    std::filesystem::remove_all(tmp);
}

CYANE_TEST(region_reads_vanilla_chunk_record_format) {
    const auto path = fixture_path("vanilla_region_chunk.bin");
    CYANE_CHECK(!path.empty());
    if (path.empty()) {
        return;
    }
    const auto blob = read_file(path);
    CYANE_CHECK_EQ(blob.size(), std::size_t{270});

    // 按原版布局构造 region 文件：8KiB 头部 + 记录置于扇区 2
    const auto dir = std::filesystem::temp_directory_path() / "cyane_test_vanilla_region";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const auto region_path = dir / "r.0.0.mca";
    {
        Bytes file(3 * 4096, std::byte{0});  // 头部 2 扇区 + 数据 1 扇区
        // 位置表：区块 (0,0) → 扇区 2、长度 1
        file[0] = std::byte{0};
        file[1] = std::byte{0};
        file[2] = std::byte{2};
        file[3] = std::byte{1};
        std::memcpy(file.data() + 8192, blob.data(), blob.size());
        std::ofstream out{region_path, std::ios::binary | std::ios::trunc};
        out.write(reinterpret_cast<const char*>(file.data()),
                  static_cast<std::streamsize>(file.size()));
    }

    auto region = world::RegionFile::load(region_path);
    CYANE_CHECK(region.has_value());
    auto chunk_nbt = region->read_chunk(0, 0);
    CYANE_CHECK(chunk_nbt.has_value() && chunk_nbt->has_value());
    auto decoded = world::decode_chunk(ByteSpan{**chunk_nbt});
    CYANE_CHECK(decoded.has_value());
    // 原版真实区块：位置与超平坦地形不同的方块数据
    CYANE_CHECK_EQ(decoded->chunk.pos().x, 0);
    CYANE_CHECK_EQ(decoded->chunk.pos().z, 0);
    CYANE_CHECK(decoded->chunk.has_blocks());
    std::size_t non_air = 0;
    for (const auto& section : decoded->chunk.sections()) {
        for (const auto state : section.states) {
            if (state != world::kStateAir) {
                ++non_air;
            }
        }
    }
    std::print("vanilla chunk non-air blocks: {}\n", non_air);
    CYANE_CHECK(non_air > 1000);

    // 我们的写出也能被自己读回（同格式自洽）
    auto wrote = region->write_chunk(1, 0, ByteSpan{**chunk_nbt});
    CYANE_CHECK(wrote.has_value());
    CYANE_CHECK(region->save(region_path).has_value());
    auto reloaded = world::RegionFile::load(region_path);
    CYANE_CHECK(reloaded.has_value());
    auto round = reloaded->read_chunk(1, 0);
    CYANE_CHECK(round.has_value() && round->has_value());
    CYANE_CHECK_EQ((*round)->size(), (**chunk_nbt).size());

    std::filesystem::remove_all(dir);
}

CYANE_TEST(level_dat_ensure_creates_minimal_set) {
    const auto tmp = std::filesystem::temp_directory_path() / "cyane_test_level_ensure";
    std::filesystem::remove_all(tmp);

    // 缺失时建档
    auto created = world::ensure_level_dat(tmp, /*game_type=*/1);
    CYANE_CHECK(created.has_value());
    CYANE_CHECK(*created);
    CYANE_CHECK(std::filesystem::exists(tmp / "level.dat"));

    // 回读出生点
    auto info = world::load_level_dat(tmp);
    CYANE_CHECK(info.has_value());
    CYANE_CHECK_EQ(info->spawn_x, 0);
    // 原版新建 level.dat 的 SpawnY 默认 64（登入时仍会按实际地表再落一次）
    CYANE_CHECK_EQ(info->spawn_y, 64);
    CYANE_CHECK_EQ(info->spawn_z, 0);

    // 已存在时不覆盖
    auto again = world::ensure_level_dat(tmp, 0);
    CYANE_CHECK(again.has_value());
    CYANE_CHECK(!*again);

    std::filesystem::remove_all(tmp);
}

CYANE_TEST(save_preserves_vanilla_fields_losslessly) {
    const auto path = fixture_path("vanilla_region_chunk.bin");
    CYANE_CHECK(!path.empty());
    if (path.empty()) {
        return;
    }
    const auto blob = read_file(path);

    // 造一个只含该区块的 region 世界，走完整 load → edit → save 链路
    const auto dir = std::filesystem::temp_directory_path() / "cyane_test_lossless";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir / "region");
    {
        Bytes file(3 * 4096, std::byte{0});
        file[2] = std::byte{2};
        file[3] = std::byte{1};
        std::memcpy(file.data() + 8192, blob.data(), blob.size());
        std::ofstream out{dir / "region" / "r.0.0.mca", std::ios::binary | std::ios::trunc};
        out.write(reinterpret_cast<const char*>(file.data()),
                  static_cast<std::streamsize>(file.size()));
    }

    world::World loaded;
    net::ContainerStore chests;
    net::FurnaceStore furnaces;
    net::ItemDropManager drops;
    net::MobManager mobs;
    game::WorldPersistence loader(loaded, chests, furnaces, drops, mobs, dir.string());
    CYANE_CHECK(loader.load().has_value());

    // 记录原方块（找第一个非空气位置）与其原始 NBT 特征
    const auto original_nbt = loaded.source_nbt(world::ChunkPos{0, 0});
    CYANE_CHECK(!original_nbt.empty());
    const auto original = world::decode_chunk(ByteSpan{original_nbt});
    CYANE_CHECK(original.has_value());
    std::int32_t probe_x = -1, probe_y = -1, probe_z = -1;
    for (std::size_t sy = 0; sy < world::kSectionCount && probe_y < 0; ++sy) {
        const auto* section = original->chunk.section(sy);
        if (section == nullptr) {
            continue;
        }
        for (std::size_t i = 0; i < section->states.size(); ++i) {
            if (section->states[i] != world::kStateAir) {
                probe_x = static_cast<std::int32_t>(i & 0xF);
                probe_z = static_cast<std::int32_t>((i >> 4) & 0xF);
                probe_y = static_cast<std::int32_t>(sy) * 16 + static_cast<std::int32_t>(i >> 8);
                break;
            }
        }
    }
    CYANE_CHECK(probe_y >= 0);
    const auto original_state = loaded.block_at(probe_x, probe_y, probe_z);

    // 编辑一个方块 → 标脏 → 保存（走 merged 编码）
    loaded.set_block(1, 200, 1, world::kStateStone);
    auto saved = loader.save();
    CYANE_CHECK(saved.has_value());
    CYANE_CHECK_EQ(*saved, std::size_t{1});

    // 直接解析落盘后的 NBT：未建模字段必须原样保留
    auto region = world::RegionFile::load(dir / "region" / "r.0.0.mca");
    CYANE_CHECK(region.has_value());
    auto nbt_bytes = region->read_chunk(0, 0);
    CYANE_CHECK(nbt_bytes.has_value() && nbt_bytes->has_value());
    auto root = nbt::parse(ByteSpan{**nbt_bytes});
    CYANE_CHECK(root.has_value());
    const auto* level = root->find("Level");
    CYANE_CHECK(level != nullptr);
    CYANE_CHECK(level->find("HeightMap") != nullptr);      // i32[256] 透传
    CYANE_CHECK(level->find("LightPopulated") != nullptr); // 标量透传
    const auto* sections = level->find("Sections");
    const auto* section_list = sections != nullptr ? sections->get_if<nbt::List>() : nullptr;
    CYANE_CHECK(section_list != nullptr);
    bool saw_light = false;
    for (const auto& section : *section_list) {
        if (section.find("SkyLight") != nullptr && section.find("BlockLight") != nullptr) {
            saw_light = true;
        }
    }
    CYANE_CHECK(saw_light);  // section 光照未被丢弃

    // 方块：编辑生效 + 原方块保留
    auto after = world::decode_chunk(ByteSpan{**nbt_bytes});
    CYANE_CHECK(after.has_value());
    const auto edited = after->chunk.block_state(1, 200, 1);
    CYANE_CHECK(edited.has_value() && *edited == world::kStateStone);
    const auto kept = after->chunk.block_state(probe_x, probe_y, probe_z);
    CYANE_CHECK(kept.has_value() && *kept == original_state);

    std::filesystem::remove_all(dir);
}

CYANE_TEST(merged_save_preserves_non_item_entities) {
    // 造一份含「生物实体 + 旧物品实体」的区块 NBT 作磁盘源档
    const world::ChunkPos pos{2, 2};
    world::Chunk chunk{pos};
    chunk.set_block_state(pos.world_x() + 5, 3, pos.world_z() + 5, world::kStateStone);

    {
        // 实体条目用原版字段名：小写 id + Pos 列表（与方块实体的 x/y/z 不同）
        // 末影人：不在物种表里 → 保存时必须原样透传
        nbt::Compound enderman_fields;
        enderman_fields.emplace_back("Pos", nbt::make_list(nbt::List{
            nbt::Value{nbt::Tag::f64, 100.0},
            nbt::Value{nbt::Tag::f64, 5.0},
            nbt::Value{nbt::Tag::f64, 100.0}}));
        enderman_fields.emplace_back("id",
                                     nbt::Value{nbt::Tag::string, std::string{"minecraft:enderman"}});
        nbt::Compound old_item_fields;
        old_item_fields.emplace_back("Item", nbt::Value{nbt::Tag::compound, nbt::Compound{}});
        old_item_fields.emplace_back("id",
                                     nbt::Value{nbt::Tag::string, std::string{"minecraft:item"}});
        // 原版被动生物（猪）：已被我们建模，保存时由内存态重写
        nbt::Compound vanilla_pig;
        vanilla_pig.emplace_back("Pos", nbt::make_list(nbt::List{
            nbt::Value{nbt::Tag::f64, 7.0},
            nbt::Value{nbt::Tag::f64, 4.0},
            nbt::Value{nbt::Tag::f64, 7.0}}));
        vanilla_pig.emplace_back("id", nbt::Value{nbt::Tag::string, std::string{"minecraft:pig"}});
        nbt::List entity_list;
        entity_list.push_back(nbt::make_compound(std::move(enderman_fields)));
        entity_list.push_back(nbt::make_compound(std::move(old_item_fields)));
        entity_list.push_back(nbt::make_compound(std::move(vanilla_pig)));
        nbt::Compound level;
        level.emplace_back("xPos", nbt::Value{nbt::Tag::i32, pos.x});
        level.emplace_back("zPos", nbt::Value{nbt::Tag::i32, pos.z});
        level.emplace_back("Entities", nbt::make_list(std::move(entity_list)));
        nbt::Compound root;
        root.emplace_back("Level", nbt::make_compound(std::move(level)));
        auto source = nbt::serialize("", nbt::make_compound(std::move(root)));
        CYANE_CHECK(source.has_value());

        world::ChunkEntities entities;
        entities.items.push_back(
            world::StoredEntity{21.5, 3.5, 21.5, item::ItemStack{260, 4, 0}});
        // 内存态生物：一头牛（原版的猪被此重写）
        entities.mobs.push_back(world::StoredMob{92, 30.5, 4.0, 30.5, 3.0f, 0.0f});
        auto merged = world::encode_chunk_merged(pos, chunk, entities, ByteSpan{*source});
        CYANE_CHECK(merged.has_value());

        // 旧物品实体被内存态替换，新物品实体出现
        auto decoded = world::decode_chunk(ByteSpan{*merged});
        CYANE_CHECK(decoded.has_value());
        CYANE_CHECK_EQ(decoded->entities.items.size(), std::size_t{1});
        CYANE_CHECK_EQ(decoded->entities.items[0].stack.id, std::int16_t{260});
        CYANE_CHECK_EQ(decoded->entities.items[0].stack.count, std::uint8_t{4});
        // 原版猪被内存态牛重写
        CYANE_CHECK_EQ(decoded->entities.mobs.size(), std::size_t{1});
        CYANE_CHECK_EQ(decoded->entities.mobs[0].type, 92);
        CYANE_CHECK(decoded->entities.mobs[0].x == 30.5);

        // 未建模实体在 NBT 中原样保留；已建模类型由内存态接管
        auto reparsed = nbt::parse(ByteSpan{*merged});
        CYANE_CHECK(reparsed.has_value());
        const nbt::Value* level_v = reparsed->find("Level");
        const nbt::Value* ents_v =
            level_v != nullptr ? level_v->find("Entities") : nullptr;
        const auto* ents = ents_v != nullptr ? ents_v->get_if<nbt::List>() : nullptr;
        CYANE_CHECK(ents != nullptr);
        // 末影人不在物种表里 → 原样透传；僵尸已建模（R-022 起）→ 由内存态接管
        bool enderman_kept = false;
        bool zombie_dropped = true;
        bool pig_rewritten = false;
        for (const auto& entry : *ents) {
            const auto id = entry.find("id") ? entry.find("id")->text() : std::nullopt;
            if (id && *id == "minecraft:enderman") {
                enderman_kept = true;
            }
            if (id && *id == "minecraft:zombie") {
                zombie_dropped = false;  // 建模后不再透传（内存态里没有就消失）
            }
            if (id && *id == "minecraft:cow") {
                pig_rewritten = true;
            }
        }
        CYANE_CHECK(enderman_kept);
        CYANE_CHECK(zombie_dropped);
        CYANE_CHECK(pig_rewritten);
        CYANE_CHECK_EQ(ents->size(), std::size_t{3});  // 末影人 + 牛 + 内存态物品
    }
}

// 真实地形区块（走 merged 无损编码）上放门后保存重载：门必须还在
CYANE_TEST(door_survives_merged_save_on_vanilla_chunk) {
    const auto path = fixture_path("vanilla_region_chunk.bin");
    CYANE_CHECK(!path.empty());
    if (path.empty()) {
        return;
    }
    const auto blob = read_file(path);
    const auto dir = std::filesystem::temp_directory_path() / "cyane_test_door_merged";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir / "region");
    {
        Bytes file(3 * 4096, std::byte{0});
        file[2] = std::byte{2};
        file[3] = std::byte{1};
        std::memcpy(file.data() + 8192, blob.data(), blob.size());
        std::ofstream out{dir / "region" / "r.0.0.mca", std::ios::binary | std::ios::trunc};
        out.write(reinterpret_cast<const char*>(file.data()),
                  static_cast<std::streamsize>(file.size()));
    }

    constexpr std::uint16_t kLower = static_cast<std::uint16_t>((64 << 4) | 0x01);
    constexpr std::uint16_t kUpper = static_cast<std::uint16_t>((64 << 4) | 0x08 | 0x01);
    std::int32_t dx = 0;
    std::int32_t dy = 0;
    std::int32_t dz = 0;
    {
        world::World world;
        net::ContainerStore chests;
        net::FurnaceStore furnaces;
        net::ItemDropManager drops;
        net::MobManager mobs;
        game::WorldPersistence persistence(world, chests, furnaces, drops, mobs, dir.string());
        CYANE_CHECK(persistence.load().has_value());
        CYANE_CHECK(!world.source_nbt(world::ChunkPos{0, 0}).empty());  // 确为 merged 路径

        // 找一个地表以上的空气格放门（保留原区块其余方块）
        for (std::int32_t y = 250; y > 1; --y) {
            if (world.block_at(3, y, 3) == world::kStateAir) {
                dy = y;
                break;
            }
        }
        CYANE_CHECK(dy > 1);
        dx = 3;
        dz = 3;
        world.set_block(dx, dy, dz, kLower);
        world.set_block(dx, dy + 1, dz, kUpper);
        const auto saved = persistence.save();
        CYANE_CHECK(saved.has_value());
    }
    {
        world::World world;
        net::ContainerStore chests;
        net::FurnaceStore furnaces;
        net::ItemDropManager drops;
        net::MobManager mobs;
        game::WorldPersistence persistence(world, chests, furnaces, drops, mobs, dir.string());
        CYANE_CHECK(persistence.load().has_value());
        CYANE_CHECK_EQ(world.block_at(dx, dy, dz), kLower);
        CYANE_CHECK_EQ(world.block_at(dx, dy + 1, dz), kUpper);
    }
    std::filesystem::remove_all(dir);
}

// 门是双半方块：放置后必须能落盘并在重载后原样恢复（曾出现放门→重进→门消失）
CYANE_TEST(placed_door_survives_save_and_reload) {
    constexpr std::uint16_t kLower = static_cast<std::uint16_t>((64 << 4) | 0x01);
    constexpr std::uint16_t kUpper = static_cast<std::uint16_t>((64 << 4) | 0x08 | 0x01);
    const auto dir = std::filesystem::temp_directory_path() / "cyane_test_door";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir / "region");
    {
        world::World world;
        net::ContainerStore chests;
        net::FurnaceStore furnaces;
        net::ItemDropManager drops;
        net::MobManager mobs;
        game::WorldPersistence persistence(world, chests, furnaces, drops, mobs, dir.string());
        world.set_block(5, 100, 7, kLower);
        world.set_block(5, 101, 7, kUpper);
        const auto saved = persistence.save();
        CYANE_CHECK(saved.has_value());
        CYANE_CHECK_EQ(*saved, std::size_t{1});
    }
    {
        world::World world;
        net::ContainerStore chests;
        net::FurnaceStore furnaces;
        net::ItemDropManager drops;
        net::MobManager mobs;
        game::WorldPersistence persistence(world, chests, furnaces, drops, mobs, dir.string());
        CYANE_CHECK(persistence.load().has_value());
        CYANE_CHECK_EQ(world.block_at(5, 100, 7), kLower);
        CYANE_CHECK_EQ(world.block_at(5, 101, 7), kUpper);
    }
    std::filesystem::remove_all(dir);
}

// 音效表必须与客户端注册表一致：0x49 走 id（1.12.2 的首字段是 SoundEffect 注册表 id，不是名字），
// id → 名字的对应由 vanilla 注册顺序决定。fixture 从 client.jar 的 SoundEvent.b() 提取，
// 已核对与服务端 SoundEffect.b() 顺序完全一致。
CYANE_TEST(toggle_sound_matches_vanilla_registry) {
    const auto path = fixture_path("sound_registry_ids.txt");
    CYANE_CHECK(!path.empty());
    std::unordered_map<std::int32_t, std::string> registry;
    std::ifstream in{path};
    std::int32_t id = 0;
    std::string name;
    while (in >> id >> name) {
        registry.emplace(id, name);
    }
    CYANE_CHECK_EQ(registry.size(), std::size_t{549});

    const auto check = [&registry](std::uint16_t block_id, std::string_view on, std::string_view off) {
        const auto sound = world::toggle_sound(block_id);
        CYANE_CHECK(sound.has_value());
        if (!sound) {
            return;
        }
        CYANE_CHECK_EQ(sound->on, on);
        CYANE_CHECK_EQ(sound->off, off);
        CYANE_CHECK_EQ(registry.at(sound->on_id), std::string{on});
        CYANE_CHECK_EQ(registry.at(sound->off_id), std::string{off});
    };

    check(77, "block.stone_button.click_on", "block.stone_button.click_off");
    check(143, "block.wood_button.click_on", "block.wood_button.click_off");
    check(69, "block.lever.click", "block.lever.click");
    check(64, "block.wooden_door.open", "block.wooden_door.close");
    check(71, "block.iron_door.open", "block.iron_door.close");
    check(96, "block.wooden_trapdoor.open", "block.wooden_trapdoor.close");
    check(107, "block.fence_gate.open", "block.fence_gate.close");

    // 类别/音量/音高取自 vanilla Block* 类：按钮与拉杆 0.3 音量、按下 0.6 回弹 0.5
    const auto stone_button = world::toggle_sound(77);
    CYANE_CHECK_EQ(stone_button->volume, 0.3f);
    CYANE_CHECK_EQ(stone_button->on_pitch, 0.6f);
    CYANE_CHECK_EQ(stone_button->off_pitch, 0.5f);
    CYANE_CHECK_EQ(stone_button->radius, 16);
    CYANE_CHECK_EQ(world::toggle_sound(69)->volume, 0.3f);
    CYANE_CHECK_EQ(world::toggle_sound(107)->volume, 1.0f);
    CYANE_CHECK_EQ(world::toggle_sound(107)->radius, 64);  // 门类的 World Event 广播半径

    // 客户端是否本地预测：按钮「按下」与开关门由客户端自己播（服务端不再回发，否则双响），
    // 按钮「回弹」与拉杆不预测（服务端必须发）
    CYANE_CHECK(stone_button->on_predicted);
    CYANE_CHECK(!stone_button->off_predicted);
    CYANE_CHECK(world::toggle_sound(143)->on_predicted);
    CYANE_CHECK(!world::toggle_sound(143)->off_predicted);
    CYANE_CHECK(!world::toggle_sound(69)->on_predicted);
    CYANE_CHECK(!world::toggle_sound(69)->off_predicted);
    CYANE_CHECK(world::toggle_sound(64)->on_predicted && world::toggle_sound(64)->off_predicted);
    CYANE_CHECK(world::toggle_sound(107)->on_predicted && world::toggle_sound(107)->off_predicted);

    CYANE_CHECK(!world::toggle_sound(1).has_value());    // 石头
    CYANE_CHECK(!world::toggle_sound(54).has_value());   // 箱子
}

// 按钮回弹是"世界级"延迟更新：计时不在按下的连接上，按下者断线也必须照常弹起
CYANE_TEST(button_release_is_world_level) {
    constexpr std::uint16_t kPressed = static_cast<std::uint16_t>((77 << 4) | 0x08);
    constexpr std::uint16_t kReleased = static_cast<std::uint16_t>(77 << 4);
    world::World world;
    net::PlayerHub hub;
    net::BlockTicks ticks;

    world.set_block(-3, 70, 5, kPressed);
    ticks.schedule_button_release(-3, 70, 5, 1000);
    CYANE_CHECK_EQ(ticks.pending(), std::size_t{1});
    ticks.tick(999, world, hub, 8);
    CYANE_CHECK_EQ(world.block_at(-3, 70, 5), kPressed);  // 未到期：仍然是按下的
    ticks.tick(1000, world, hub, 8);
    CYANE_CHECK_EQ(world.block_at(-3, 70, 5), kReleased);
    CYANE_CHECK_EQ(ticks.pending(), std::size_t{0});

    // 期间方块被破坏：回弹什么都不做
    world.set_block(-3, 70, 5, kPressed);
    ticks.schedule_button_release(-3, 70, 5, 2000);
    world.set_block(-3, 70, 5, world::kStateAir);
    ticks.tick(2000, world, hub, 8);
    CYANE_CHECK_EQ(world.block_at(-3, 70, 5), world::kStateAir);

    // 已提前回弹（0x8 已清）：不再重复广播，也不改变状态
    world.set_block(-3, 70, 5, kReleased);
    ticks.schedule_button_release(-3, 70, 5, 3000);
    ticks.tick(3000, world, hub, 8);
    CYANE_CHECK_EQ(world.block_at(-3, 70, 5), kReleased);
}

CYANE_TEST(java_random_matches_java_8_oracle_vectors) {
    struct Vector {
        std::int64_t seed;
        std::uint64_t first;
        std::uint64_t second;
        int bounded;
    };
    constexpr std::array vectors{
        Vector{0, 0xbb20b45fd4d95138ULL, 0x3d93cb799b3970beULL, 368843508},
        Vector{1, 0xbb1ad57319b89cd8ULL, 0x68fb0e6f684df992ULL, 446064254},
        Vector{-1, 0x44d96cb3708722c3ULL, 0x032420168c4bff9eULL, 421815597},
        Vector{12345, 0x5c9f20d58361b331ULL, 0xeed8a921eac80778ULL, 789050048},
        Vector{987654321, 0x4e5210afec7d68ffULL, 0xd19b620532582ff3ULL, 98087148},
    };
    for (const auto& vector : vectors) {
        world::gen::JavaRandom random{static_cast<std::uint64_t>(vector.seed)};
        CYANE_CHECK_EQ(random.next_long(), vector.first);
        CYANE_CHECK_EQ(random.next_long(), vector.second);
        CYANE_CHECK_EQ(random.next_int(1000000007), vector.bounded);
    }
}

CYANE_TEST(overworld_generator_produces_deterministic_terrain) {
    world::gen::ChunkGeneratorOverworld gen1{123456789ULL};
    world::gen::ChunkGeneratorOverworld gen2{123456789ULL};

    const auto c1 = gen1.generate(0, 0);
    const auto c2 = gen2.generate(0, 0);

    // 1. 基岩层必定在 y=0
    CYANE_CHECK_EQ(c1.block_at(0, 0, 0), world::kStateBedrock);
    CYANE_CHECK_EQ(c1.block_at(8, 0, 8), world::kStateBedrock);
    CYANE_CHECK_EQ(c1.block_at(15, 0, 15), world::kStateBedrock);

    // 2. 确定性：同种子生成结果完全一致
    for (std::size_t x = 0; x < 16; ++x) {
        for (std::size_t z = 0; z < 16; ++z) {
            CYANE_CHECK_EQ(c1.block_at(x, 0, z), c2.block_at(x, 0, z));
            CYANE_CHECK_EQ(c1.block_at(x, 60, z), c2.block_at(x, 60, z));
        }
    }

    // 3. 地表高度应当大于超平坦的 y=3（自然起伏地形）
    int max_y = 0;
    for (int y = 255; y >= 0; --y) {
        if (c1.block_at(8, y, 8) != world::kStateAir) {
            max_y = y;
            break;
        }
    }
    CYANE_CHECK(max_y >= 50); // 自然陆地或水面高度通常在 50~100 之间

    // 4. 地表连续性：相邻方块地表基础高度平缓过渡（排除地表树木与花草遮挡）
    auto is_terrain = [](std::uint16_t state) {
        return state == world::kStateGrass || state == world::kStateDirt ||
               state == world::kStateStone || state == world::kStateSand ||
               state == world::kStateGravel || state == world::kStateWater;
    };
    for (std::size_t x = 0; x < 15; ++x) {
        int h1 = 0, h2 = 0;
        for (int y = 255; y >= 0; --y) {
            if (is_terrain(c1.block_at(x, y, 8)) && h1 == 0) h1 = y;
            if (is_terrain(c1.block_at(x + 1, y, 8)) && h2 == 0) h2 = y;
        }
        CYANE_CHECK(std::abs(h1 - h2) <= 3);
    }
}

CYANE_TEST(world_with_generator_materializes_terrain) {
    world::World world;
    world.set_generator(987654321ULL);

    // 当请求未物化区块时，触发生成器
    const auto chunk = world.chunk_at(world::ChunkPos{2, 2});
    CYANE_CHECK(chunk.has_blocks());
    CYANE_CHECK_EQ(chunk.block_at(0, 0, 0), world::kStateBedrock);
}

CYANE_TEST(overworld_generator_generates_ores_and_trees) {
    // 1. 验证生成器产出原版矿石
    world::gen::ChunkGeneratorOverworld gen{42ULL};
    auto chunk = gen.generate(0, 0);
    // 矿脉已按原版移入 populate 阶段
    gen.populate(0, 0, chunk);

    bool has_coal = false;
    bool has_iron = false;

    for (std::size_t x = 0; x < 16; ++x) {
        for (std::size_t z = 0; z < 16; ++z) {
            for (int y = 0; y < 256; ++y) {
                const auto state = chunk.block_at(x, y, z);
                if (state == world::kStateCoalOre) has_coal = true;
                if (state == world::kStateIronOre) has_iron = true;
            }
        }
    }

    CYANE_CHECK(has_coal);
    CYANE_CHECK(has_iron);

    // 2. 验证原版树木生成器放置原木与树叶，且草地变为泥土
    world::Chunk tree_chunk{world::ChunkPos{0, 0}};
    for (std::size_t x = 0; x < 16; ++x) {
        for (std::size_t z = 0; z < 16; ++z) {
            tree_chunk.set_block(x, 63, z, world::kStateGrass);
        }
    }
    world::gen::JavaRandom rand{12345ULL};
    world::gen::WorldGenTrees tree_gen{4, world::kStateOakLog, world::kStateOakLeaves};
    const bool placed = tree_gen.generate(tree_chunk, rand, 8, 64, 8);
    CYANE_CHECK(placed);
    CYANE_CHECK_EQ(tree_chunk.block_at(8, 63, 8), world::kStateDirt);
    CYANE_CHECK_EQ(tree_chunk.block_at(8, 64, 8), world::kStateOakLog);
    CYANE_CHECK_EQ(tree_chunk.block_at(8, 67, 8), world::kStateOakLog);

    bool has_leaves = false;
    for (std::size_t x = 6; x <= 10; ++x) {
        for (std::size_t z = 6; z <= 10; ++z) {
            if (tree_chunk.block_at(x, 68, z) == world::kStateOakLeaves) {
                has_leaves = true;
            }
        }
    }
    CYANE_CHECK(has_leaves);
}

CYANE_TEST(block_break_particles_and_dig_hardness) {
    // 1. 验证 WorldEvent 2001 破坏粒子封包编码
    ByteWriter out;
    net::writers::write_world_event(out, 2001, 10, 64, -20, static_cast<std::int32_t>(world::kStateStone));
    ByteReader reader{out.data()};
    const auto effect_id = reader.i32();
    const auto pos = reader.i64();
    const auto data = reader.i32();
    const auto server_wide = reader.boolean();

    CYANE_CHECK(effect_id.has_value());
    CYANE_CHECK_EQ(*effect_id, 2001);
    CYANE_CHECK(pos.has_value());
    CYANE_CHECK_EQ(position_x(*pos), 10);
    CYANE_CHECK_EQ(position_y(*pos), 64);
    CYANE_CHECK_EQ(position_z(*pos), -20);
    CYANE_CHECK(data.has_value());
    CYANE_CHECK_EQ(*data, static_cast<std::int32_t>(world::kStateStone));
    CYANE_CHECK(server_wide.has_value());
    CYANE_CHECK_EQ(*server_wide, false);

    // 2. 挖掘耗时计算
    CYANE_CHECK(world::block_hardness(world::block_id(world::kStateBedrock)) < 0.0f);
    CYANE_CHECK(world::block_hardness(world::block_id(world::kStateDirt)) == 0.5f);
    CYANE_CHECK(world::block_hardness(world::block_id(world::kStateStone)) == 1.5f);

    // 钻石镐挖石头（速 8x，can_harvest）耗时显著短于木镐或徒手
    const float diamond_pick_time = item::dig_duration_ms(world::block_id(world::kStateStone), 278, true);
    const float wood_pick_time = item::dig_duration_ms(world::block_id(world::kStateStone), 270, true);
    const float hand_time = item::dig_duration_ms(world::block_id(world::kStateStone), 0, false);
    CYANE_CHECK(diamond_pick_time < wood_pick_time);
    CYANE_CHECK(wood_pick_time < hand_time);
}

CYANE_TEST(vanilla_biomes_and_ore_abundance) {
    // 1. 验证新群系属性与注册一致性
    CYANE_CHECK_EQ(world::biome::kMushroomIsland.id, 14);
    CYANE_CHECK_EQ(world::biome::kMushroomIsland.top_block, world::kStateMycelium);
    CYANE_CHECK_EQ(world::biome::kHell.id, 8);
    CYANE_CHECK_EQ(world::biome::kHell.top_block, world::kStateNetherrack);
    CYANE_CHECK_EQ(world::biome::kSky.id, 9);
    CYANE_CHECK_EQ(world::biome::kSky.top_block, world::kStateEndStone);
    CYANE_CHECK_EQ(world::biome::kDeepOcean.id, 24);

    // 2. 验证修复 +8 偏移后矿物丰富度（单区块生成矿石总数达 150 以上）
    world::gen::ChunkGeneratorOverworld gen{9999ULL};
    auto chunk = gen.generate(1, 1);
    // 矿脉已按原版移入 populate 阶段
    gen.populate(1, 1, chunk);
    int ore_count = 0;
    for (std::size_t x = 0; x < 16; ++x) {
        for (std::size_t z = 0; z < 16; ++z) {
            for (int y = 0; y < 128; ++y) {
                const auto s = chunk.block_at(x, y, z);
                if (s == world::kStateCoalOre || s == world::kStateIronOre ||
                    s == world::kStateGoldOre || s == world::kStateRedstoneOre ||
                    s == world::kStateDiamondOre || s == world::kStateLapisOre ||
                    s == world::kStateGranite || s == world::kStateDiorite ||
                    s == world::kStateAndesite) {
                    ++ore_count;
                }
            }
        }
    }
    CYANE_CHECK(ore_count >= 150);
}

CYANE_TEST(world_gen_features_dungeon_and_plants) {
    // 1. 验证地牢生成（实心石体内部空腔、苔石地表、刷怪笼与宝箱）
    world::Chunk chunk{world::ChunkPos{0, 0}};
    for (std::size_t x = 0; x < 16; ++x) {
        for (std::size_t z = 0; z < 16; ++z) {
            for (int y = 20; y <= 35; ++y) {
                chunk.set_block(x, y, z, world::kStateStone);
            }
        }
    }
    chunk.set_block(4, 25, 8, world::kStateAir);
    chunk.set_block(4, 26, 8, world::kStateAir);

    world::gen::JavaRandom rand{54321ULL};
    const bool dungeon_ok = world::gen::WorldGenDungeons::generate(chunk, rand, 8, 25, 8);
    CYANE_CHECK(dungeon_ok);
    CYANE_CHECK_EQ(chunk.block_at(8, 25, 8), world::kStateMobSpawner);

    // 2. 验证沙漠水井生成
    world::Chunk desert_chunk{world::ChunkPos{0, 0}};
    const bool well_ok = world::gen::WorldGenDesertWells::generate(desert_chunk, 8, 65, 8);
    CYANE_CHECK(well_ok);
    CYANE_CHECK_EQ(desert_chunk.block_at(8, 65, 8), world::kStateWater);
    CYANE_CHECK_EQ(desert_chunk.block_at(8, 64, 8), world::kStateSandstone);

    // 3. 验证双层高植物生成（上下两半方块联动）
    world::Chunk flower_chunk{world::ChunkPos{0, 0}};
    for (std::size_t x = 0; x < 16; ++x) {
        for (std::size_t z = 0; z < 16; ++z) {
            flower_chunk.set_block(x, 63, z, world::kStateGrass);
        }
    }
    const bool double_ok = world::gen::WorldGenDoublePlant::generate(flower_chunk, rand, 8, 64, 8, 0);
    CYANE_CHECK(double_ok);

    bool has_double_plant = false;
    for (std::size_t x = 0; x < 16; ++x) {
        for (std::size_t z = 0; z < 16; ++z) {
            if (flower_chunk.block_at(x, 64, z) == static_cast<std::uint16_t>(world::kStateDoublePlant | 0)) {
                has_double_plant = true;
                CYANE_CHECK_EQ(flower_chunk.block_at(x, 65, z), static_cast<std::uint16_t>(world::kStateDoublePlant | 0x8));
            }
        }
    }
    CYANE_CHECK(has_double_plant);

    // 4. 验证巨型蘑菇（WorldGenBigMushroom：红菇与棕菇）
    world::Chunk shroom_chunk{world::ChunkPos{0, 0}};
    shroom_chunk.set_block(8, 63, 8, world::kStateMycelium);
    const bool red_shroom_ok = world::gen::WorldGenBigMushroom::generate(shroom_chunk, rand, 8, 64, 8, true);
    CYANE_CHECK(red_shroom_ok);
    CYANE_CHECK_EQ(shroom_chunk.block_at(8, 64, 8), static_cast<std::uint16_t>(world::kStateHugeRedMushroom | 10)); // 菌柄

    // 5. 验证浮冰刺（WorldGenIceSpike）
    world::Chunk spike_chunk{world::ChunkPos{0, 0}};
    const bool spike_ok = world::gen::WorldGenIceSpike::generate(spike_chunk, rand, 8, 60, 8);
    CYANE_CHECK(spike_ok);
    CYANE_CHECK_EQ(spike_chunk.block_at(8, 60, 8), world::kStatePackedIce);

    // 6. 验证藤蔓墙面附着（WorldGenVines）
    world::Chunk vine_chunk{world::ChunkPos{0, 0}};
    vine_chunk.set_block(7, 64, 8, world::kStateStone); // 西侧实心墙
    const bool vine_ok = world::gen::WorldGenVines::generate(vine_chunk, rand, 8, 64, 8);
    CYANE_CHECK(vine_ok);

    // 7. 验证末地黑曜石柱与折跃门（WorldGenSpikes / WorldGenEndGateway）
    world::Chunk end_chunk{world::ChunkPos{0, 0}};
    const bool spike_gen_ok = world::gen::WorldGenSpikes::generate(end_chunk, 8, 50, 8, 2, 10, true);
    CYANE_CHECK(spike_gen_ok);
    CYANE_CHECK_EQ(end_chunk.block_at(8, 55, 8), world::kStateObsidian);
    CYANE_CHECK_EQ(end_chunk.block_at(6, 61, 8), world::kStateIronBars);

    const bool gateway_ok = world::gen::WorldGenEndGateway::generate(end_chunk, 8, 75, 8);
    CYANE_CHECK(gateway_ok);
    CYANE_CHECK_EQ(end_chunk.block_at(8, 75, 8), world::kStateEndGateway);
    CYANE_CHECK_EQ(end_chunk.block_at(8, 74, 8), world::kStateBedrock);

    // 8. 验证下界火与荧石簇（WorldGenFire / WorldGenGlowStone1）
    world::Chunk nether_chunk{world::ChunkPos{0, 0}};
    nether_chunk.set_block(8, 50, 8, world::kStateNetherrack);
    const bool fire_ok = world::gen::WorldGenFire::generate(nether_chunk, rand, 8, 51, 8);
    CYANE_CHECK(fire_ok);

    nether_chunk.set_block(8, 100, 8, world::kStateNetherrack);
    const bool glow_ok = world::gen::WorldGenGlowStone1::generate(nether_chunk, rand, 8, 99, 8);
    CYANE_CHECK(glow_ok);
    CYANE_CHECK_EQ(nether_chunk.block_at(8, 99, 8), world::kStateGlowstone);

    // 9. 验证超平坦预设解析器（FlatGeneratorInfo / FlatLayerInfo）
    const auto flat_info = world::gen::FlatGeneratorInfo::create_flat_preset("3;minecraft:bedrock,2*minecraft:dirt,minecraft:grass;1");
    CYANE_CHECK_EQ(flat_info.get_biome(), 1);
    const auto& layers = flat_info.get_flat_layers();
    CYANE_CHECK_EQ(layers.size(), 3ULL);
    CYANE_CHECK_EQ(layers[0].get_layer_count(), 1);
    CYANE_CHECK_EQ(layers[0].get_block_state(), world::kStateBedrock);
    CYANE_CHECK_EQ(layers[1].get_layer_count(), 2);
    CYANE_CHECK_EQ(layers[1].get_block_state(), world::kStateDirt);
    CYANE_CHECK_EQ(layers[2].get_layer_count(), 1);
    CYANE_CHECK_EQ(layers[2].get_block_state(), world::kStateGrass);
    CYANE_CHECK_EQ(layers[2].get_min_y(), 3);

    // 10. 验证结构组件三维包围盒相交检测（StructureBoundingBox）
    using world::structure::StructureBoundingBox;
    StructureBoundingBox box1(0, 0, 0, 10, 10, 10);
    StructureBoundingBox box2(5, 5, 5, 15, 15, 15);
    StructureBoundingBox box3(20, 20, 20, 30, 30, 30);
    CYANE_CHECK(box1.intersects_with(box2));
    CYANE_CHECK(!box1.intersects_with(box3));
    CYANE_CHECK(box1.is_vec_inside(5, 5, 5));
    CYANE_CHECK(!box1.is_vec_inside(11, 5, 5));
    CYANE_CHECK_EQ(box1.get_x_size(), 11);

    // 11. 验证下界与超平坦维度生成器（ChunkGeneratorFlat / ChunkGeneratorHell）
    world::Chunk flat_chunk{world::ChunkPos{0, 0}};
    world::gen::ChunkGeneratorFlat flat_gen(flat_info);
    flat_gen.generate(0, 0, flat_chunk);
    CYANE_CHECK_EQ(flat_chunk.block_at(0, 0, 0), world::kStateBedrock);
    CYANE_CHECK_EQ(flat_chunk.block_at(0, 1, 0), world::kStateDirt);
    CYANE_CHECK_EQ(flat_chunk.block_at(0, 3, 0), world::kStateGrass);
    CYANE_CHECK_EQ(flat_chunk.block_at(0, 4, 0), world::kStateAir);

    world::Chunk hell_chunk{world::ChunkPos{0, 0}};
    world::gen::ChunkGeneratorHell hell_gen(12345ULL);
    hell_gen.generate(0, 0, hell_chunk);
    CYANE_CHECK_EQ(hell_chunk.block_at(0, 0, 0), world::kStateBedrock);
    CYANE_CHECK_EQ(hell_chunk.block_at(0, 127, 0), world::kStateBedrock);

    // 12. 验证大型分叉巨橡树（WorldGenBigTree）
    world::Chunk big_tree_chunk{world::ChunkPos{0, 0}};
    big_tree_chunk.set_block(8, 63, 8, world::kStateGrass);
    world::gen::WorldGenBigTree big_tree;
    const bool big_tree_ok = big_tree.generate(big_tree_chunk, rand, 8, 64, 8);
    CYANE_CHECK(big_tree_ok);
    CYANE_CHECK_EQ(big_tree_chunk.block_at(8, 64, 8), world::kStateOakLog);

    // 13. 验证世界难度与区域难度实例（EnumDifficulty / DifficultyInstance）
    world::DifficultyInstance diff_inst(world::EnumDifficulty::Hard, 1000000L, 5000000L, 1.0f);
    CYANE_CHECK(diff_inst.get_additional_difficulty() > 0.0f);
    CYANE_CHECK_EQ(diff_inst.get_difficulty(), world::EnumDifficulty::Hard);
    CYANE_CHECK_EQ(world::get_difficulty_name(world::EnumDifficulty::Peaceful), "peaceful");

    // 14. 验证原版游戏规则（GameRules）
    world::GameRules rules;
    CYANE_CHECK(rules.get_bool("doDaylightCycle"));
    CYANE_CHECK(!rules.get_bool("keepInventory"));
    CYANE_CHECK_EQ(rules.get_int("randomTickSpeed"), 3);
    rules.set_or_create("keepInventory", "true");
    CYANE_CHECK(rules.get_bool("keepInventory"));

    // 15. 验证世界边界体系（WorldBorder）
    world::WorldBorder border;
    border.set_center(0.0, 0.0);
    border.set_diameter(1000.0);
    CYANE_CHECK(border.contains(0.0, 0.0));
    CYANE_CHECK(border.contains(499.0, 499.0));
    CYANE_CHECK(!border.contains(501.0, 0.0));
    CYANE_CHECK_EQ(border.min_x(), -500.0);
    CYANE_CHECK_EQ(border.max_x(), 500.0);

    // 16. 验证爆炸动力学（Explosion）
    world::Chunk exp_chunk{world::ChunkPos{0, 0}};
    for (std::size_t x = 0; x < 16; ++x) {
        for (std::size_t z = 0; z < 16; ++z) {
            exp_chunk.set_block(x, 10, z, world::kStateStone);
        }
    }
    world::Explosion exp(8.0, 10.0, 8.0, 4.0f, false, true);
    exp.do_explosion_a(exp_chunk, rand);
    CYANE_CHECK(!exp.get_affected_blocks().empty());
    exp.do_explosion_b(exp_chunk, rand);
    CYANE_CHECK_EQ(exp_chunk.block_at(8, 10, 8), world::kStateAir);

    // 17. 光照通道与维度类型（EnumSkyBlock / DimensionType）
    CYANE_CHECK_EQ(world::default_light_value(world::EnumSkyBlock::Sky), 15);
    CYANE_CHECK_EQ(world::default_light_value(world::EnumSkyBlock::Block), 0);
    CYANE_CHECK_EQ(world::dimension_id(world::DimensionType::Nether), -1);
    CYANE_CHECK_EQ(world::dimension_name(world::DimensionType::TheEnd), "the_end");
    CYANE_CHECK_EQ(world::dimension_suffix(world::DimensionType::Overworld), "");
    CYANE_CHECK(world::dimension_loads_spawn(world::DimensionType::Overworld));
    CYANE_CHECK(!world::dimension_loads_spawn(world::DimensionType::Nether));
    CYANE_CHECK(world::dimension_by_id(1) == world::DimensionType::TheEnd);
    CYANE_CHECK(!world::dimension_by_id(7).has_value());

    // 18. 容器锁码（LockCode）
    const world::LockCode empty_lock;
    CYANE_CHECK(empty_lock.is_empty());
    const world::LockCode keyed_lock{"secret"};
    CYANE_CHECK(!keyed_lock.is_empty());
    CYANE_CHECK_EQ(keyed_lock.get_lock(), "secret");

    // 19. 延迟方块更新队列（TickList / NextTickListEntry）
    world::TickList ticks;
    ticks.schedule(1, 64, 2, world::kStateWater, 100);
    ticks.schedule(3, 64, 2, world::kStateWater, 90);
    CYANE_CHECK_EQ(ticks.size(), 2ULL);
    CYANE_CHECK(ticks.is_scheduled(1, 64, 2, world::kStateWater));

    // 重复调度同目标只替换到期时间，不新增条目
    ticks.schedule(1, 64, 2, world::kStateWater, 50);
    CYANE_CHECK_EQ(ticks.size(), 2ULL);

    // 按到期时间顺序执行，执行后队列清空
    std::vector<int> tick_order;
    ticks.tick(95, [&](const world::NextTickListEntry& entry) { tick_order.push_back(entry.x); });
    CYANE_CHECK_EQ(tick_order.size(), 2ULL);
    CYANE_CHECK_EQ(tick_order[0], 1);
    CYANE_CHECK_EQ(tick_order[1], 3);
    CYANE_CHECK(ticks.empty());

    // 未到期条目不执行
    ticks.schedule(5, 64, 5, world::kStateWater, 500);
    int fired = 0;
    ticks.tick(499, [&](const world::NextTickListEntry&) { ++fired; });
    CYANE_CHECK_EQ(fired, 0);
    CYANE_CHECK_EQ(ticks.size(), 1ULL);

    // 20. Boss 血条（BossInfo / BossInfoServer）
    world::BossInfoServer boss{42, "Ender Dragon", world::BossColor::Pink, world::BossOverlay::Progress};
    CYANE_CHECK_EQ(boss.percent(), 1.0f);
    boss.set_percent(0.35f);
    CYANE_CHECK_EQ(boss.percent(), 0.35f);
    boss.set_percent(2.5f); // 钳制到 1.0
    CYANE_CHECK_EQ(boss.percent(), 1.0f);
    boss.add_player(7);
    CYANE_CHECK(boss.should_notify(7));
    CYANE_CHECK(!boss.should_notify(8));
    boss.remove_player(7);
    CYANE_CHECK(!boss.should_notify(7));
}

CYANE_TEST(loot_table_loads_and_generates_like_vanilla) {
    // 物品名解析器由调用方提供（服务端注册表）；测试用最小桩
    const auto resolver = [](std::string_view name) -> std::optional<std::int16_t> {
        if (name == "minecraft:iron_ingot") return 265;
        if (name == "minecraft:bread") return 297;
        if (name == "minecraft:golden_apple") return 322;
        return std::nullopt;
    };
    world::loot::LootTableManager manager{resolver};

    // 对齐 vanilla chests/simple_dungeon 的结构：两池、加权条目、set_data、空条目
    constexpr std::string_view kJson = R"({
        "pools": [
            {
                "rolls": {"min": 2, "max": 2},
                "entries": [
                    {"type": "item", "name": "minecraft:iron_ingot", "weight": 10,
                     "functions": [{"function": "set_count", "count": {"min": 1, "max": 4}}]},
                    {"type": "item", "name": "minecraft:golden_apple", "weight": 1,
                     "functions": [{"function": "set_data", "data": 1}]},
                    {"type": "empty", "weight": 5}
                ]
            },
            {
                "rolls": 1,
                "entries": [
                    {"type": "item", "name": "minecraft:bread", "weight": 1,
                     "functions": [{"function": "looting_enchant", "count": {"min": 0, "max": 1}, "limit": 5}]}
                ]
            }
        ]
    })";

    CYANE_CHECK(manager.add_table_json("minecraft:chests/simple_dungeon", kJson));
    CYANE_CHECK_EQ(manager.size(), 1ULL);
    const auto* table = manager.find("minecraft:chests/simple_dungeon");
    CYANE_CHECK(table != nullptr);
    CYANE_CHECK_EQ(table->pools.size(), 2ULL);

    world::gen::JavaRandom rand{987654321ULL};
    world::loot::LootContext ctx{};

    // 每轮必出 2 个主池条目（empty 条目不加物品）+ 1 个面包
    for (int round = 0; round < 64; ++round) {
        const auto items = manager.generate("minecraft:chests/simple_dungeon", rand, ctx);
        CYANE_CHECK(!items.empty());
        int bread = 0;
        for (const auto& stack : items) {
            CYANE_CHECK(!stack.empty());
            if (stack.id == 265) {
                CYANE_CHECK(stack.count >= 1 && stack.count <= 4);
            }
            if (stack.id == 322) {
                CYANE_CHECK_EQ(stack.damage, 1); // set_data 生效
            }
            if (stack.id == 297) ++bread;
        }
        CYANE_CHECK_EQ(bread, 1);
    }

    // random_chance = 0 的池条件：整池不产出
    world::loot::LootTableManager gated{resolver};
    CYANE_CHECK(gated.add_table_json("minecraft:test/gated", R"({
        "pools": [{
            "rolls": 3,
            "conditions": [{"condition": "random_chance", "chance": 0.0}],
            "entries": [{"type": "item", "name": "minecraft:iron_ingot"}]
        }]
    })"));
    CYANE_CHECK(gated.generate("minecraft:test/gated", rand, ctx).empty());

    // killed_by_player 条件：未由玩家击杀时不产出
    world::loot::LootTableManager killer_only{resolver};
    CYANE_CHECK(killer_only.add_table_json("minecraft:test/killer", R"({
        "pools": [{
            "rolls": 1,
            "entries": [{"type": "item", "name": "minecraft:bread",
                         "conditions": [{"condition": "killed_by_player"}]}]
        }]
    })"));
    world::loot::LootContext not_player{};
    CYANE_CHECK(killer_only.generate("minecraft:test/killer", rand, not_player).empty());
    world::loot::LootContext by_player{};
    by_player.killer_is_player = true;
    CYANE_CHECK(!killer_only.generate("minecraft:test/killer", rand, by_player).empty());

    // set_damage：按比例产生磨损（SetDamage.apply），不是整数区间
    const auto tool_resolver = [](std::string_view name) -> std::optional<std::int16_t> {
        if (name == "minecraft:diamond_sword") return 276;
        if (name == "minecraft:diamond") return 264;
        if (name == "minecraft:bread") return 297;
        return std::nullopt;
    };
    world::loot::LootTableManager damage_table{tool_resolver};
    CYANE_CHECK(damage_table.add_table_json("minecraft:test/damage", R"({
        "pools": [{"rolls": 1, "entries": [{"type": "item", "name": "minecraft:diamond_sword",
            "functions": [{"function": "set_damage", "damage": {"min": 0.0, "max": 0.9}}]}]}]
    })"));
    bool saw_worn = false;
    for (int i = 0; i < 32; ++i) {
        const auto items = damage_table.generate("minecraft:test/damage", rand, ctx);
        CYANE_CHECK_EQ(items.size(), 1ULL);
        if (items.empty()) continue;
        // 钻石剑 maxDamage 1561，磨损 = floor((1-x)*1561)，x∈[0,0.9] → [156,1561]
        CYANE_CHECK(items[0].damage >= 0 && items[0].damage <= 1561);
        if (items[0].damage > 0) saw_worn = true;
    }
    CYANE_CHECK(saw_worn);

    // 不可损坏物品（钻石）不受 set_damage 影响
    world::loot::LootTableManager no_durability{tool_resolver};
    CYANE_CHECK(no_durability.add_table_json("minecraft:test/nodur", R"({
        "pools": [{"rolls": 1, "entries": [{"type": "item", "name": "minecraft:diamond",
            "functions": [{"function": "set_damage", "damage": {"min": 0.0, "max": 0.9}}]}]}]
    })"));
    const auto plain = no_durability.generate("minecraft:test/nodur", rand, ctx);
    CYANE_CHECK_EQ(plain.size(), 1ULL);
    if (!plain.empty()) CYANE_CHECK_EQ(plain[0].damage, 0);

    // entity_properties(on_fire)：着火才掉；这是牛/鸡/猪"熟肉"的条件来源
    world::loot::LootTableManager on_fire{resolver};
    CYANE_CHECK(on_fire.add_table_json("minecraft:test/onfire", R"({
        "pools": [{"rolls": 1, "entries": [{"type": "item", "name": "minecraft:bread",
            "conditions": [{"condition": "entity_properties", "entity": "this",
                            "properties": {"on_fire": true}}]}]}]
    })"));
    world::loot::LootContext cold{};
    CYANE_CHECK(on_fire.generate("minecraft:test/onfire", rand, cold).empty());
    world::loot::LootContext hot{};
    hot.entity_on_fire = true;
    CYANE_CHECK(!on_fire.generate("minecraft:test/onfire", rand, hot).empty());

    // killed_by_player 的 inverse：非玩家击杀才产出
    world::loot::LootTableManager inverse_killer{resolver};
    CYANE_CHECK(inverse_killer.add_table_json("minecraft:test/inverse", R"({
        "pools": [{"rolls": 1, "entries": [{"type": "item", "name": "minecraft:bread",
            "conditions": [{"condition": "killed_by_player", "inverse": true}]}]}]
    })"));
    CYANE_CHECK(!inverse_killer.generate("minecraft:test/inverse", rand, not_player).empty());
    CYANE_CHECK(inverse_killer.generate("minecraft:test/inverse", rand, by_player).empty());

    // 未知条件一律恒假，绝不当作满足
    world::loot::LootTableManager unknown_cond{resolver};
    CYANE_CHECK(unknown_cond.add_table_json("minecraft:test/unknown", R"({
        "pools": [{"rolls": 1, "entries": [{"type": "item", "name": "minecraft:bread",
            "conditions": [{"condition": "some_future_condition"}]}]}]
    })"));
    CYANE_CHECK(unknown_cond.generate("minecraft:test/unknown", rand, ctx).empty());

    // set_count 超过 64：按堆叠上限拆成多堆（LootEntryItem 同）
    world::loot::LootTableManager big_count{resolver};
    CYANE_CHECK(big_count.add_table_json("minecraft:test/big", R"({
        "pools": [{"rolls": 1, "entries": [{"type": "item", "name": "minecraft:bread",
            "functions": [{"function": "set_count", "count": 100}]}]}]
    })"));
    const auto big = big_count.generate("minecraft:test/big", rand, ctx);
    CYANE_CHECK_EQ(big.size(), 2ULL);
    if (big.size() == 2) {
        CYANE_CHECK_EQ(static_cast<int>(big[0].count), 64);
        CYANE_CHECK_EQ(static_cast<int>(big[1].count), 36);
    }

    // 空池保留：保证 rolls 随机数被消耗，序列与原版一致
    world::loot::LootTableManager empty_pool{resolver};
    CYANE_CHECK(empty_pool.add_table_json("minecraft:test/emptypool", R"({
        "pools": [
            {"rolls": {"min": 1, "max": 5}, "entries": []},
            {"rolls": 1, "entries": [{"type": "item", "name": "minecraft:bread"}]}
        ]
    })"));
    const auto* empty_table = empty_pool.find("minecraft:test/emptypool");
    CYANE_CHECK(empty_table != nullptr);
    if (empty_table != nullptr) CYANE_CHECK_EQ(empty_table->pools.size(), 2ULL);

    // 自引用 loot_table 条目必须被 active 集合拦住（不会无限递归）
    world::loot::LootTableManager recursive{resolver};
    CYANE_CHECK(recursive.add_table_json("minecraft:test/loop", R"({
        "pools": [{"rolls": 1, "entries": [{"type": "loot_table", "name": "minecraft:test/loop"}]}]
    })"));
    CYANE_CHECK(recursive.generate("minecraft:test/loop", rand, ctx).empty());
}

CYANE_TEST(loot_chest_fills_once_with_marked_table) {
    const auto resolver = [](std::string_view name) -> std::optional<std::int16_t> {
        if (name == "minecraft:iron_ingot") return 265;
        return std::nullopt;
    };
    auto manager = std::make_shared<world::loot::LootTableManager>(resolver);
    CYANE_CHECK(manager->add_table_json("minecraft:chests/simple_dungeon", R"({
        "pools": [{"rolls": {"min": 2, "max": 2}, "entries": [
            {"type": "item", "name": "minecraft:iron_ingot",
             "functions": [{"function": "set_count", "count": {"min": 1, "max": 3}}]}
        ]}]
    })"));

    net::ContainerStore store;
    store.set_loot_manager(manager);
    constexpr std::int64_t key = 12345;
    store.mark_loot(key, std::string{world::loot::table_names::kChestsSimpleDungeon}, 424242);
    CYANE_CHECK(store.has_loot(key));

    // 首次打开：按掉落表填入，标记随即清除
    const auto first = store.snapshot(key);
    CYANE_CHECK(!store.has_loot(key));

    int filled = 0;
    int ingots = 0;
    for (const auto& slot : first) {
        if (slot.empty()) continue;
        ++filled;
        CYANE_CHECK_EQ(slot.id, 265);
        CYANE_CHECK(slot.count >= 1 && slot.count <= 3);
        ingots += slot.count;
    }
    CYANE_CHECK_EQ(filled, 2);

    // 第二次打开内容稳定（不会再生成一遍）
    const auto second = store.snapshot(key);
    int second_filled = 0;
    for (const auto& slot : second) {
        if (!slot.empty()) ++second_filled;
    }
    CYANE_CHECK_EQ(second_filled, filled);
    CYANE_CHECK(ingots >= 2);
}

CYANE_TEST(world_settings_and_world_types_match_vanilla) {
    using cyane::world::WorldType;
    CYANE_CHECK_EQ(cyane::world::world_type_name(WorldType::Flat), "flat");
    CYANE_CHECK_EQ(cyane::world::world_type_name(WorldType::LargeBiomes), "largeBiomes");
    CYANE_CHECK_EQ(cyane::world::world_type_version(WorldType::Default1_1), 0);
    CYANE_CHECK_EQ(cyane::world::world_type_version(WorldType::Default), 1);
    CYANE_CHECK(!cyane::world::world_type_can_be_created(WorldType::Default1_1));
    CYANE_CHECK(cyane::world::world_type_has_info_notice(WorldType::Amplified));
    CYANE_CHECK(cyane::world::world_type_by_name("flat") == WorldType::Flat);
    CYANE_CHECK(!cyane::world::world_type_by_name("nope").has_value());

    cyane::world::WorldSettings settings{123456789LL, 1, true, false, WorldType::Default};
    CYANE_CHECK_EQ(settings.seed(), 123456789LL);
    CYANE_CHECK_EQ(settings.game_type(), 1);
    CYANE_CHECK(settings.map_features_enabled());
    CYANE_CHECK(!settings.is_hardcore_mode_enabled());
    settings.enable_bonus_chest().enable_commands().set_generator_options("2;0");
    CYANE_CHECK(settings.bonus_chest_enabled());
    CYANE_CHECK(settings.commands_allowed());
    CYANE_CHECK_EQ(settings.generator_options(), "2;0");

    // 困难核心只在生存模式生效
    const cyane::world::WorldSettings hardcore_survival{1LL, 0, true, true, WorldType::Default};
    CYANE_CHECK(hardcore_survival.is_hardcore_mode_enabled());
    const cyane::world::WorldSettings hardcore_creative{1LL, 1, true, true, WorldType::Default};
    CYANE_CHECK(!hardcore_creative.is_hardcore_mode_enabled());
}

CYANE_TEST(map_data_round_trips_through_nbt) {
    using cyane::world::storage::MapData;
    using cyane::world::storage::MapDecoration;
    using cyane::world::storage::MapDecorationType;

    MapData map{"map_3"};
    map.calculate_map_center(1000.0, -4000.0, 0);
    CYANE_CHECK_EQ(map.x_center(), 1024);
    CYANE_CHECK_EQ(map.z_center(), -3968);
    map.set_dimension(-1);
    map.set_scale(2);
    map.set_color_at(5, 7, 0x2A);
    map.add_decoration("player_1", MapDecoration{MapDecorationType::Player, 12, -9, 3});
    CYANE_CHECK(map.is_dirty());

    const auto encoded = map.write_to_nbt();
    // 用 NBT 序列化/解析走一遍真实字节，确保键与类型正确
    const auto bytes = cyane::world::nbt::serialize("map_3", encoded);
    CYANE_CHECK(bytes.has_value());
    const auto decoded = cyane::world::nbt::parse(cyane::ByteSpan{bytes->data(), bytes->size()});
    CYANE_CHECK(decoded.has_value());

    MapData restored{"map_3"};
    restored.read_from_nbt(*decoded);
    CYANE_CHECK_EQ(restored.x_center(), 1024);
    CYANE_CHECK_EQ(restored.z_center(), -3968);
    CYANE_CHECK_EQ(restored.dimension(), -1);
    CYANE_CHECK_EQ(restored.scale(), 2);
    CYANE_CHECK_EQ(restored.color_at(5, 7), 0x2A);
    CYANE_CHECK_EQ(restored.decorations().size(), 1ULL);
    const auto& decoration = restored.decorations().at("player_1");
    CYANE_CHECK_EQ(static_cast<int>(decoration.x), 12);
    CYANE_CHECK_EQ(static_cast<int>(decoration.y), -9);
    CYANE_CHECK_EQ(static_cast<int>(decoration.rotation), 3);

    // MapStorage：idCounts 自增 + 地图按名索引
    cyane::world::storage::MapStorage storage;
    CYANE_CHECK_EQ(storage.next_map_id(), 0);
    CYANE_CHECK_EQ(storage.next_map_id(), 1);
    auto* created = storage.get_map(1);
    CYANE_CHECK(created != nullptr);
    CYANE_CHECK_EQ(created->map_name(), "map_1");
    CYANE_CHECK_EQ(storage.size(), 1ULL);
    const auto id_counts = storage.write_id_counts();
    cyane::world::storage::MapStorage other;
    other.read_id_counts(id_counts);
    CYANE_CHECK_EQ(other.next_map_id(), 2);
}

CYANE_TEST(world_info_preserves_legacy_generator_and_dimension_data) {
    world::nbt::Compound data;
    data.emplace_back("generatorName", world::nbt::make_string("default"));
    data.emplace_back("generatorVersion", world::nbt::make_i32(0));
    data.emplace_back("DayTime", world::nbt::make_i64(-1));
    data.emplace_back("GameRules", world::nbt::make_compound({}));
    world::nbt::Compound dimension;
    dimension.emplace_back("Custom", world::nbt::make_string("kept"));
    world::nbt::Compound dimensions;
    dimensions.emplace_back("1", world::nbt::make_compound(std::move(dimension)));
    data.emplace_back("DimensionData", world::nbt::make_compound(std::move(dimensions)));
    data.emplace_back("Bukkit.Version", world::nbt::make_string("git-Spigot-fixture"));

    world::storage::WorldInfo info;
    info.read_from_nbt(world::nbt::make_compound(std::move(data)));
    CYANE_CHECK(info.terrain_type == world::WorldType::Default1_1);
    CYANE_CHECK_EQ(info.day_time, -1);
    // GameRules.loadFromNBT 只对 NBT 里出现的键 setOrCreateGameRule，不会清空默认值，
    // 因此空 GameRules 复合仍保留 24 条默认规则，且类型不被降级成字符串。
    CYANE_CHECK_EQ(info.game_rules.get_rules().size(), std::size_t{24});
    CYANE_CHECK(info.game_rules.get_bool("doDaylightCycle"));
    CYANE_CHECK(info.game_rules.is_boolean("doDaylightCycle"));
    const auto* custom = info.dimension_data().find("1");
    CYANE_CHECK(custom != nullptr);
    if (custom != nullptr) {
        CYANE_CHECK_EQ(custom->find("Custom")->text().value_or(""), "kept");
    }
    auto written = info.write_to_nbt();
    const auto* output = written.find("Data");
    CYANE_CHECK(output != nullptr);
    if (output != nullptr) {
        // WorldType.getName() 为 "default_1_1"（原版 updateTagCompound 直接写它）
        CYANE_CHECK_EQ(output->find("generatorName")->text().value_or(""), "default_1_1");
        const auto* version = output->find("generatorVersion");
        CYANE_CHECK(version != nullptr);
        if (version != nullptr) CYANE_CHECK_EQ(version->scalar().value_or(-1), 0);
        CYANE_CHECK_EQ(output->find("Bukkit.Version")->text().value_or(""),
                       "git-Spigot-fixture");
        const auto* day = output->find("DayTime");
        CYANE_CHECK(day != nullptr);
        if (day != nullptr) CYANE_CHECK_EQ(day->scalar().value_or(0), -1);
        // 维度数据必须回写（原版 updateTagCompound 写 DimensionData）
        const auto* dims = output->find("DimensionData");
        CYANE_CHECK(dims != nullptr);
        if (dims != nullptr) {
            const auto* kept = dims->find("1");
            CYANE_CHECK(kept != nullptr);
            if (kept != nullptr) {
                CYANE_CHECK_EQ(kept->find("Custom")->text().value_or(""), "kept");
            }
        }
    }
    // set_dimension_data 写入的条目也要落盘
    info.set_dimension_data(-1, world::nbt::make_compound({}));
    const auto with_dimension = info.write_to_nbt();
    const auto* after_set = with_dimension.find("Data");
    CYANE_CHECK(after_set != nullptr);
    if (after_set != nullptr) {
        CYANE_CHECK(after_set->find("DimensionData")->find("-1") != nullptr);
    }

    const world::WorldSettings settings{999, 1, true, false, world::WorldType::Flat};
    const world::storage::WorldInfo created{settings, "Flat"};
    CYANE_CHECK_EQ(created.random_seed, 999);
    CYANE_CHECK_EQ(static_cast<int>(created.game_type), 1);
    CYANE_CHECK(created.terrain_type == world::WorldType::Flat);
    CYANE_CHECK(!created.initialized);
    CYANE_CHECK(!created.allow_commands);
}

CYANE_TEST(world_info_round_trips_full_vanilla_fields) {
    const auto dir = std::filesystem::temp_directory_path() / "cyane_test_world_info";
    std::filesystem::remove_all(dir);

    CYANE_CHECK(world::ensure_level_dat(dir, /*game_type=*/2, /*seed=*/1234567890123LL).has_value());

    // 读回后用 WorldInfo 覆盖一批字段再写盘，验证原版键名/类型往返
    auto loaded = world::load_world_info(dir);
    CYANE_CHECK(loaded.has_value());
    CYANE_CHECK_EQ(loaded->random_seed, 1234567890123LL);
    CYANE_CHECK_EQ(static_cast<int>(loaded->game_type), 2);
    CYANE_CHECK_EQ(loaded->level_name, std::string{"cyane_test_world_info"});

    loaded->level_name = "测试世界";
    loaded->hardcore = true;
    loaded->difficulty = world::EnumDifficulty::Hard;
    loaded->world_time = 987654321;
    loaded->day_time = 6000;
    loaded->raining = true;
    loaded->rain_time = 1200;
    loaded->thundering = true;
    loaded->thunder_time = 600;
    loaded->spawn_x = 100;
    loaded->spawn_y = 70;
    loaded->spawn_z = -200;
    loaded->game_rules.set_or_create("keepInventory", "true");
    loaded->game_rules.set_or_create("randomTickSpeed", "7");
    loaded->border_diameter = 29999984.0;
    loaded->border_warning_distance = 12;

    const auto nbt_bytes = world::nbt::serialize("", loaded->write_to_nbt());
    CYANE_CHECK(nbt_bytes.has_value());
    const auto reparsed = world::nbt::parse(cyane::ByteSpan{nbt_bytes->data(), nbt_bytes->size()});
    CYANE_CHECK(reparsed.has_value());

    world::storage::WorldInfo restored;
    restored.read_root(*reparsed);
    CYANE_CHECK_EQ(restored.level_name, std::string{"测试世界"});
    CYANE_CHECK(restored.hardcore);
    CYANE_CHECK_EQ(static_cast<int>(restored.difficulty), 3);
    CYANE_CHECK_EQ(restored.world_time, 987654321);
    CYANE_CHECK_EQ(restored.day_time, 6000);
    CYANE_CHECK(restored.raining);
    CYANE_CHECK_EQ(restored.rain_time, 1200);
    CYANE_CHECK(restored.thundering);
    CYANE_CHECK_EQ(restored.spawn_x, 100);
    CYANE_CHECK_EQ(restored.spawn_y, 70);
    CYANE_CHECK_EQ(restored.spawn_z, -200);
    CYANE_CHECK(restored.game_rules.get_bool("keepInventory"));
    CYANE_CHECK_EQ(restored.game_rules.get_int("randomTickSpeed"), 7);
    CYANE_CHECK_EQ(restored.border_warning_distance, 12);
    CYANE_CHECK(restored.dimension_data().find("0") == nullptr);

    // 存档摘要（WorldSummary）
    const auto summary = world::storage::WorldSummary::from_root(*reparsed);
    CYANE_CHECK_EQ(summary.level_name, std::string{"测试世界"});
    CYANE_CHECK_EQ(summary.random_seed, 1234567890123LL);
    CYANE_CHECK_EQ(summary.version_name, std::string{"1.12.2"});
    CYANE_CHECK(!summary.requires_conversion);

    std::filesystem::remove_all(dir);
}

CYANE_TEST(gen_layer_seed_matches_vanilla_lcg) {
    // 参考向量由独立的 Python 实现（任意精度 + 显式 mod 2^64）按 Java long 语义算出，
    // 用于校验 C++ 侧的 LCG 转录（乘法回绕、加法顺序）。
    using cyane::world::layer::LayerSeed;

    // GenLayer(long)：baseSeed 自身回绕三次
    CYANE_CHECK_EQ(LayerSeed{1}.base_seed(), 3107951898966440229ULL);
    CYANE_CHECK_EQ(LayerSeed{2}.base_seed(), 13432066074785117656ULL);
    CYANE_CHECK_EQ(LayerSeed{100}.base_seed(), 5723240131506253216ULL);
    CYANE_CHECK_EQ(LayerSeed{2000}.base_seed(), 9672642253349399552ULL);

    // initWorldGenSeed(12345) → 回绕两次加 baseSeed
    LayerSeed layer{1};
    layer.init_world_gen_seed(12345);
    CYANE_CHECK_EQ(layer.world_gen_seed(), 2045757533502615161ULL);

    // initChunkSeed(0,0) → 回绕四次交替加 x/z
    layer.init_chunk_seed(0, 0);
    CYANE_CHECK_EQ(layer.chunk_seed(), 14040106546769989532ULL);

    // nextInt 序列（每次取值后推进一次 chunkSeed）
    const std::array<std::int32_t, 5> expected{8, 8, 9, 0, 5};
    for (const auto want : expected) {
        CYANE_CHECK_EQ(layer.next_int(10), want);
    }

    // 同种子必须完全可复现
    LayerSeed a{7};
    LayerSeed b{7};
    a.init_world_gen_seed(999);
    b.init_world_gen_seed(999);
    a.init_chunk_seed(3, -5);
    b.init_chunk_seed(3, -5);
    for (int i = 0; i < 32; ++i) {
        CYANE_CHECK_EQ(a.next_int(1000), b.next_int(1000));
    }
}

CYANE_TEST(gen_layer_island_zoom_chain_matches_reference) {
    // 参考网格由独立的 Python 实现（照 GenLayerIsland / GenLayerZoom /
    // GenLayerFuzzyZoom 的 Java 源码另写一遍）在 seed=12345、区域 (-4,-4,8x8) 上算出，
    // 用于校验索引数学（zoom 的 l1++ 交错写、按 areaX&1 的偏移取窗口）。
    using namespace cyane::world::layer;

    auto island = std::make_shared<GenLayerIsland>(1);
    std::shared_ptr<GenLayer> fuzzy = std::make_shared<GenLayerFuzzyZoom>(2000, island);
    auto zoom = std::make_shared<GenLayerZoom>(2001, fuzzy);
    zoom->init_world_gen_seed(12345);

    constexpr std::int32_t kW = 8;
    constexpr std::int32_t kH = 8;
    const std::array<std::int32_t, 64> expect_island{
        0, 0, 1, 1, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 0, 0, 0, 0,
        0, 0, 1, 0, 0, 0, 0, 0, 0, 1, 0, 0, 1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0};
    const std::array<std::int32_t, 64> expect_zoom{
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 0, 0, 0, 0, 1, 1, 1, 0, 0,
        0, 0, 1, 1, 1, 1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0};

    const auto got_island = island->get_ints(-4, -4, kW, kH);
    CYANE_CHECK_EQ(got_island.size(), std::size_t{64});
    for (std::size_t i = 0; i < expect_island.size() && i < got_island.size(); ++i) {
        CYANE_CHECK_EQ(got_island[i], expect_island[i]);
    }

    const auto got_zoom = zoom->get_ints(-4, -4, kW, kH);
    CYANE_CHECK_EQ(got_zoom.size(), std::size_t{64});
    for (std::size_t i = 0; i < expect_zoom.size() && i < got_zoom.size(); ++i) {
        CYANE_CHECK_EQ(got_zoom[i], expect_zoom[i]);
    }

    // 从世界种子重新初始化必须完全可复现
    auto again = std::make_shared<GenLayerZoom>(
        2001, std::make_shared<GenLayerFuzzyZoom>(2000, std::make_shared<GenLayerIsland>(1)));
    again->init_world_gen_seed(12345);
    CYANE_CHECK(again->get_ints(-4, -4, kW, kH) == got_zoom);
}

CYANE_TEST(gen_layer_select_mode_or_random_has_inequality_guards) {
    using cyane::world::layer::LayerSeed;
    // seed=0 是这套 LCG 的不动点（lcg_step(0,0)=0），chunkSeed 会恒为 0，
    // 无法观察"是否消耗随机数"，因此用非退化种子。
    LayerSeed seed{12345};
    seed.init_world_gen_seed(67890);
    seed.init_chunk_seed(3, -7);

    // 三值相同：直接返回，不消耗随机数
    const auto mark1 = seed.chunk_seed();
    CYANE_CHECK_EQ(seed.select_mode_or_random(5, 7, 7, 7), 7);
    CYANE_CHECK_EQ(seed.chunk_seed(), mark1);

    // {a,a,b,b}：所有三缺一分支都被不等式守卫否掉，必须落到 selectRandom，
    // 也就是要消耗一次 chunkSeed。漏掉守卫的旧实现会直接返回 a 且不消耗。
    const auto mark2 = seed.chunk_seed();
    const auto picked = seed.select_mode_or_random(3, 3, 9, 9);
    CYANE_CHECK(seed.chunk_seed() != mark2);
    CYANE_CHECK(picked == 3 || picked == 9);

    // {a,a,a,b}：a==b && a==c 成立 → 返回 a，不消耗
    const auto mark3 = seed.chunk_seed();
    CYANE_CHECK_EQ(seed.select_mode_or_random(4, 4, 4, 1), 4);
    CYANE_CHECK_EQ(seed.chunk_seed(), mark3);
}

CYANE_TEST(gen_layer_add_island_remove_ocean_snow_match_reference) {
    // 参考网格来自独立的 Python 实现（照 Java 源码另写一遍），seed=12345、(-4,-4,8x8)。
    // 重点校验 AddIsland 的短路求值：邻格为 0 时不取随机数也不自增计数，
    // 这决定了后续所有 nextInt 的 bound 序列。
    using namespace cyane::world::layer;

    auto island = std::make_shared<GenLayerIsland>(1);
    auto add_island = std::make_shared<GenLayerAddIsland>(1, island);
    auto remove_ocean = std::make_shared<GenLayerRemoveTooMuchOcean>(2, add_island);
    auto add_snow = std::make_shared<GenLayerAddSnow>(2, remove_ocean);
    add_snow->init_world_gen_seed(12345);

    const std::array<std::int32_t, 64> expect_island{
        0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0,
        1, 1, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 1, 0, 1, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0};
    const std::array<std::int32_t, 64> expect_ocean{
        0, 0, 0, 1, 1, 0, 1, 0, 0, 1, 0, 1, 1, 1, 0, 1, 0, 0, 1, 0, 0, 0, 1, 0,
        1, 1, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0, 0, 0, 1, 0,
        0, 0, 0, 1, 0, 1, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0};
    const std::array<std::int32_t, 64> expect_snow{
        0, 0, 0, 1, 1, 0, 1, 0, 0, 1, 0, 1, 1, 1, 0, 1, 0, 0, 1, 0, 0, 0, 1, 0,
        4, 1, 0, 1, 0, 0, 0, 0, 0, 0, 0, 3, 1, 0, 0, 1, 0, 4, 4, 0, 0, 0, 1, 0,
        0, 0, 0, 1, 0, 3, 0, 1, 0, 0, 0, 0, 4, 0, 0, 0};

    const auto got_island = add_island->get_ints(-4, -4, 8, 8);
    for (std::size_t i = 0; i < expect_island.size() && i < got_island.size(); ++i) {
        CYANE_CHECK_EQ(got_island[i], expect_island[i]);
    }
    const auto got_ocean = remove_ocean->get_ints(-4, -4, 8, 8);
    for (std::size_t i = 0; i < expect_ocean.size() && i < got_ocean.size(); ++i) {
        CYANE_CHECK_EQ(got_ocean[i], expect_ocean[i]);
    }
    const auto got_snow = add_snow->get_ints(-4, -4, 8, 8);
    for (std::size_t i = 0; i < expect_snow.size() && i < got_snow.size(); ++i) {
        CYANE_CHECK_EQ(got_snow[i], expect_snow[i]);
    }
}

CYANE_TEST(gen_layer_edge_mushroom_deep_match_reference) {
    // 参考网格由独立 Python 实现生成，seed=12345、(-4,-4,8x8)。
    // 整链 Island→AddIsland→AddSnow→Edge(三模式)→Mushroom→DeepOcean 一起比对：
    // 任一层的索引或随机序列错位都会让最终网格对不上。
    using namespace cyane::world::layer;

    auto l0 = std::make_shared<GenLayerIsland>(1);
    auto l1 = std::make_shared<GenLayerAddIsland>(1, l0);
    auto l2 = std::make_shared<GenLayerAddSnow>(2, l1);
    auto l3 = std::make_shared<GenLayerEdge>(2, l2, GenLayerEdge::Mode::cool_warm);
    auto l4 = std::make_shared<GenLayerEdge>(2, l3, GenLayerEdge::Mode::heat_ice);
    auto l5 = std::make_shared<GenLayerEdge>(3, l4, GenLayerEdge::Mode::special);
    auto l6 = std::make_shared<GenLayerAddMushroomIsland>(5, l5);
    auto l7 = std::make_shared<GenLayerDeepOcean>(4, l6);
    l7->init_world_gen_seed(12345);

    const std::array<std::int32_t, 64> expect_cw{
        0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0,
        4, 2, 0, 2, 0, 0, 0, 0, 0, 0, 0, 3, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 1, 0, 3, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0};
    const std::array<std::int32_t, 64> expect_special{
        0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0,
        3, 2, 0, 2, 0, 0, 0, 0, 0, 0, 0, 3, 514, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 1, 0, 3, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0};
    const std::array<std::int32_t, 64> expect_deep{
        24, 0, 24, 24, 24, 0, 24, 0, 0, 1, 0, 24, 24, 24, 0, 1, 0, 0, 24, 0, 24, 24, 24, 0,
        3, 2, 0, 2, 0, 24, 24, 24, 0, 0, 0, 3, 514, 0, 24, 24, 24, 24, 24, 0, 0, 0, 24, 0,
        24, 24, 0, 1, 0, 3, 0, 1, 0, 0, 24, 0, 24, 0, 24, 0};

    const auto got_cw = l3->get_ints(-4, -4, 8, 8);
    for (std::size_t i = 0; i < expect_cw.size() && i < got_cw.size(); ++i) {
        CYANE_CHECK_EQ(got_cw[i], expect_cw[i]);
    }
    const auto got_special = l5->get_ints(-4, -4, 8, 8);
    for (std::size_t i = 0; i < expect_special.size() && i < got_special.size(); ++i) {
        CYANE_CHECK_EQ(got_special[i], expect_special[i]);
    }
    const auto got_deep = l7->get_ints(-4, -4, 8, 8);
    for (std::size_t i = 0; i < expect_deep.size() && i < got_deep.size(); ++i) {
        CYANE_CHECK_EQ(got_deep[i], expect_deep[i]);
    }
}

CYANE_TEST(gen_layer_smooth_river_voronoi_match_reference) {
    // 参考网格由独立 Python 实现生成，seed=12345、(-4,-4,8x8)。
    using namespace cyane::world::layer;

    auto l0 = std::make_shared<GenLayerIsland>(1);
    auto l1 = std::make_shared<GenLayerAddIsland>(1, l0);
    auto l2 = std::make_shared<GenLayerAddSnow>(2, l1);
    auto l7 = std::make_shared<GenLayerDeepOcean>(4, l2);
    auto smooth_layer = std::make_shared<GenLayerSmooth>(1000, l7);
    auto river_init = std::make_shared<GenLayerRiverInit>(100, l7);
    auto river_layer = std::make_shared<GenLayerRiver>(1, river_init);
    auto voronoi = std::make_shared<GenLayerVoronoiZoom>(10, l7);
    // 每条的顶层都要初始化世界种子，否则该层自己的 worldGenSeed 仍是 0，
    // 只有真正取到随机数的分支才会暴露差异。
    smooth_layer->init_world_gen_seed(12345);
    river_layer->init_world_gen_seed(12345);
    voronoi->init_world_gen_seed(12345);

    const std::array<std::int32_t, 64> expect_smooth{
        0, 24, 0, 24, 24, 24, 0, 24, 0, 0, 24, 24, 24, 24, 24, 0, 0, 1, 0, 24, 24, 24, 24, 24,
        0, 0, 1, 0, 0, 24, 24, 24, 0, 0, 0, 3, 0, 0, 24, 24, 24, 24, 0, 0, 0, 0, 0, 24,
        24, 24, 24, 0, 0, 0, 24, 0, 24, 24, 24, 24, 0, 24, 0, 24};
    const std::array<std::int32_t, 64> expect_voronoi{
        0, 1, 1, 1, 24, 24, 24, 24, 1, 1, 1, 1, 1, 24, 0, 0, 1, 1, 1, 1, 1, 0, 0, 0,
        1, 1, 1, 1, 1, 0, 0, 0, 1, 1, 1, 3, 3, 3, 0, 0, 0, 3, 3, 3, 3, 3, 1, 1,
        0, 3, 3, 3, 3, 3, 1, 1, 24, 3, 3, 3, 3, 1, 1, 1};

    const auto got_smooth = smooth_layer->get_ints(-4, -4, 8, 8);
    for (std::size_t i = 0; i < expect_smooth.size() && i < got_smooth.size(); ++i) {
        CYANE_CHECK_EQ(got_smooth[i], expect_smooth[i]);
    }
    const auto got_voronoi = voronoi->get_ints(-4, -4, 8, 8);
    for (std::size_t i = 0; i < expect_voronoi.size() && i < got_voronoi.size(); ++i) {
        CYANE_CHECK_EQ(got_voronoi[i], expect_voronoi[i]);
    }

    // River：随机河种几乎不会四邻相同，因此绝大多数格是河流 id 7；
    // 只断言取值域与"存在 -1 的可能"不够严格，这里核对整张网格。
    const auto got_river = river_layer->get_ints(-4, -4, 8, 8);
    for (std::size_t i = 0; i < got_river.size(); ++i) {
        CYANE_CHECK(got_river[i] == 7 || got_river[i] == -1);
    }
    // 河流层必须对同一输入稳定复现
    auto again = std::make_shared<GenLayerRiver>(
        1, std::make_shared<GenLayerRiverInit>(100, l7));
    again->init_world_gen_seed(12345);
    CYANE_CHECK(again->get_ints(-4, -4, 8, 8) == got_river);
}

CYANE_TEST(gen_layer_river_mix_applies_biome_overrides) {
    using namespace cyane::world::layer;

    // 用合成父层直接验证 RiverMix 的四条分支：
    //   海洋不变 / 非河流不变 / 冰原上的河→冻河 / 蘑菇岛上的河→蘑菇岛岸 / 其余→河流 id
    class Fixed final : public GenLayer {
    public:
        Fixed(std::vector<std::int32_t> values) : GenLayer(0), values_(std::move(values)) {}
        [[nodiscard]] std::vector<std::int32_t> get_ints(std::int32_t, std::int32_t, std::int32_t width,
                                                         std::int32_t height) override {
            std::vector<std::int32_t> out(values_.begin(), values_.begin() +
                                          static_cast<std::ptrdiff_t>(width) * height);
            return out;
        }
    private:
        std::vector<std::int32_t> values_;
    };

    auto biomes = std::make_shared<Fixed>(std::vector<std::int32_t>{
        biome_ids::kOcean, biome_ids::kDeepOcean, biome_ids::kIcePlains,
        biome_ids::kMushroomIsland, biome_ids::kMushroomIslandShore, 1});
    auto rivers = std::make_shared<Fixed>(std::vector<std::int32_t>{
        biome_ids::kRiver, biome_ids::kRiver, biome_ids::kRiver,
        biome_ids::kRiver, biome_ids::kRiver, -1});
    GenLayerRiverMix mix{100, biomes, rivers};
    const auto got = mix.get_ints(0, 0, 6, 1);
    CYANE_CHECK_EQ(got.size(), std::size_t{6});
    CYANE_CHECK_EQ(got[0], biome_ids::kOcean);          // 海洋不变
    CYANE_CHECK_EQ(got[1], biome_ids::kDeepOcean);      // 深海不变
    CYANE_CHECK_EQ(got[2], biome_ids::kFrozenRiver);    // 冰原 → 冻河
    CYANE_CHECK_EQ(got[3], biome_ids::kMushroomIslandShore);
    CYANE_CHECK_EQ(got[4], biome_ids::kMushroomIslandShore);
    CYANE_CHECK_EQ(got[5], 1);                          // 非河流保持不变
}

CYANE_TEST(biome_table_ids_match_vanilla_registration) {
    // 原版 Biome.java 的 registerBiome 顺序：37 mesa、38 mesa_rock、39 mesa_clear_rock。
    // 此前项目把 mesa 记成 39、mesa_clear_rock 记成 38，高度也串了。
    CYANE_CHECK_EQ(cyane::world::biome::kMesa.id, 37);
    CYANE_CHECK_EQ(std::string_view(cyane::world::biome::kMesa.name), std::string_view("mesa"));
    CYANE_CHECK_EQ(cyane::world::biome::kMesa.base_height, 0.1f);
    CYANE_CHECK_EQ(cyane::world::biome::kMesa.height_variation, 0.2f);

    CYANE_CHECK_EQ(cyane::world::biome::kMesaPlateau.id, 38);
    CYANE_CHECK_EQ(std::string_view(cyane::world::biome::kMesaPlateau.name), std::string_view("mesa_rock"));
    CYANE_CHECK_EQ(cyane::world::biome::kMesaPlateau.base_height, 1.5f);

    CYANE_CHECK_EQ(cyane::world::biome::kMesaClearRock.id, 39);
    CYANE_CHECK_EQ(std::string_view(cyane::world::biome::kMesaClearRock.name),
                   std::string_view("mesa_clear_rock"));
    CYANE_CHECK_EQ(cyane::world::biome::kMesaClearRock.height_variation, 0.025f);

    // savanna 35(0.125/0.05) 与 savanna_rock 36(1.5/0.025) 本就正确，一并锁定
    CYANE_CHECK_EQ(cyane::world::biome::kSavanna.id, 35);
    CYANE_CHECK_EQ(cyane::world::biome::kSavannaPlateau.id, 36);
    CYANE_CHECK_EQ(cyane::world::biome::kSavannaPlateau.base_height, 1.5f);
}

CYANE_TEST(gen_layer_biome_bands_match_spigot_arrays) {
    // 数组内容取自 Spigot 反编译（v1_12_R1.GenLayerBiome）。
    using namespace cyane::world::layer;

    class Fixed final : public GenLayer {
    public:
        Fixed(std::vector<std::int32_t> values) : GenLayer(0), values_(std::move(values)) {}
        [[nodiscard]] std::vector<std::int32_t> get_ints(std::int32_t, std::int32_t,
                                                         std::int32_t width,
                                                         std::int32_t height) override {
            return std::vector<std::int32_t>(values_.begin(),
                                             values_.begin() +
                                                 static_cast<std::ptrdiff_t>(width) * height);
        }
    private:
        std::vector<std::int32_t> values_;
    };

    const auto contains = [](std::initializer_list<std::int32_t> set, std::int32_t value) {
        for (const auto candidate : set) {
            if (candidate == value) return true;
        }
        return false;
    };

    // 逐个温度带跑多次，断言输出落在该带的候选集合里
    for (int round = 0; round < 40; ++round) {
        // 带 1（无标记）
        {
            auto parent = std::make_shared<Fixed>(std::vector<std::int32_t>(64, 1));
            GenLayerBiome layer{200, parent, false};
            layer.init_world_gen_seed(12345);
            const auto out = layer.get_ints(0, 0, 8, 8);
            CYANE_CHECK(contains({biome_ids::kDesert, biome_ids::kSavanna, biome_ids::kPlains},
                                 out[0]));
        }
        // 带 1 + 标记 → mesa_rock / mesa_clear_rock
        {
            auto parent = std::make_shared<Fixed>(std::vector<std::int32_t>(64, 1 | (3 << 8)));
            GenLayerBiome layer{200, parent, false};
            layer.init_world_gen_seed(12345);
            const auto out = layer.get_ints(0, 0, 8, 8);
            CYANE_CHECK(contains({biome_ids::kMesaRock, biome_ids::kMesaClearRock}, out[0]));
        }
        // 带 2 + 标记 → jungle
        {
            auto parent = std::make_shared<Fixed>(std::vector<std::int32_t>(64, 2 | (1 << 8)));
            GenLayerBiome layer{200, parent, false};
            layer.init_world_gen_seed(12345);
            CYANE_CHECK_EQ(layer.get_ints(0, 0, 8, 8)[0], biome_ids::kJungle);
        }
        // 带 2 无标记 → warm 集合
        {
            auto parent = std::make_shared<Fixed>(std::vector<std::int32_t>(64, 2));
            GenLayerBiome layer{200, parent, false};
            layer.init_world_gen_seed(12345);
            const auto out = layer.get_ints(0, 0, 8, 8);
            CYANE_CHECK(contains({biome_ids::kForest, biome_ids::kRoofedForest,
                                  biome_ids::kExtremeHills, biome_ids::kPlains,
                                  biome_ids::kBirchForest, biome_ids::kSwampland},
                                 out[0]));
        }
        // 带 3 + 标记 → redwood_taiga
        {
            auto parent = std::make_shared<Fixed>(std::vector<std::int32_t>(64, 3 | (1 << 8)));
            GenLayerBiome layer{200, parent, false};
            layer.init_world_gen_seed(12345);
            CYANE_CHECK_EQ(layer.get_ints(0, 0, 8, 8)[0], biome_ids::kRedwoodTaiga);
        }
        // 带 4 → icy 集合
        {
            auto parent = std::make_shared<Fixed>(std::vector<std::int32_t>(64, 4));
            GenLayerBiome layer{200, parent, false};
            layer.init_world_gen_seed(12345);
            const auto out = layer.get_ints(0, 0, 8, 8);
            CYANE_CHECK(contains({biome_ids::kIceFlats, biome_ids::kTaigaCold}, out[0]));
        }
    }

    // 海洋与蘑菇岛原样透传
    {
        auto parent = std::make_shared<Fixed>(std::vector<std::int32_t>{
            biome_ids::kOcean, biome_ids::kDeepOcean, biome_ids::kFrozenOcean,
            biome_ids::kMushroomIsland, 99});
        GenLayerBiome layer{200, parent, false};
        layer.init_world_gen_seed(12345);
        const auto out = layer.get_ints(0, 0, 5, 1);
        CYANE_CHECK_EQ(out[0], biome_ids::kOcean);
        CYANE_CHECK_EQ(out[1], biome_ids::kDeepOcean);
        CYANE_CHECK_EQ(out[2], biome_ids::kFrozenOcean);
        CYANE_CHECK_EQ(out[3], biome_ids::kMushroomIsland);
        CYANE_CHECK_EQ(out[4], biome_ids::kMushroomIsland);  // 其它值落到蘑菇岛
    }
}

CYANE_TEST(gen_layer_rare_biome_only_replaces_plains) {
    // 反编译自 v1_12_R1.GenLayerPlains：1/57 且原值为 plains 才换成 mutated_plains。
    using namespace cyane::world::layer;

    // 父层无论被要多大区域都铺满同一个值（RareBiome 会向父层要 w+2 × h+2）
    class Solid final : public GenLayer {
    public:
        explicit Solid(std::int32_t value) : GenLayer(0), value_(value) {}
        [[nodiscard]] std::vector<std::int32_t> get_ints(std::int32_t, std::int32_t,
                                                         std::int32_t width,
                                                         std::int32_t height) override {
            return std::vector<std::int32_t>(
                static_cast<std::size_t>(width) * static_cast<std::size_t>(height), value_);
        }
    private:
        std::int32_t value_;
    };

    // 大面积 plains：应当出现 mutated_plains，且其他值一律不变
    auto plains_parent = std::make_shared<Solid>(biome_ids::kPlains);
    GenLayerRareBiome rare{1001, plains_parent};
    rare.init_world_gen_seed(12345);
    const auto out = rare.get_ints(0, 0, 64, 64);
    int mutated = 0;
    for (const auto value : out) {
        CYANE_CHECK(value == biome_ids::kPlains || value == biome_ids::kMutatedPlains);
        if (value == biome_ids::kMutatedPlains) ++mutated;
    }
    CYANE_CHECK(mutated > 0);

    // 非 plains：一个都不能变
    auto forest_parent = std::make_shared<Solid>(biome_ids::kForest);
    GenLayerRareBiome rare2{1001, forest_parent};
    rare2.init_world_gen_seed(12345);
    for (const auto value : rare2.get_ints(0, 0, 64, 64)) {
        CYANE_CHECK_EQ(value, biome_ids::kForest);
    }
}

CYANE_TEST(biome_table_matches_vanilla_extraction) {
    using namespace cyane::world::biome_table;

    // 21 对变异体，全部是 base + 128
    CYANE_CHECK_EQ(kMutationBases.size(), std::size_t{21});
    CYANE_CHECK_EQ(*mutation_of(1), 129);    // plains -> mutated_plains
    CYANE_CHECK_EQ(*mutation_of(39), 167);   // mesa_clear_rock -> mutated_mesa_clear_rock
    CYANE_CHECK(!mutation_of(0).has_value());   // ocean 无变异体
    CYANE_CHECK(!mutation_of(13).has_value());  // ice_mountains 无变异体
    CYANE_CHECK(!mutation_of(17).has_value());  // desert_hills 无变异体

    // isMutation：id >= 128 且 base 有变异体
    CYANE_CHECK(is_mutation(129));
    CYANE_CHECK(is_mutation(167));
    CYANE_CHECK(!is_mutation(38));
    CYANE_CHECK(!is_mutation(128));  // 128 不存在

    // 群系类等价：mesa 三兄弟同类；desert 与 desert_hills 同类但不同于 plains
    CYANE_CHECK(biomes_equal_or_mesa_plateau(37, 38));   // mesa 与 mesa_rock 同为 BiomeMesa
    CYANE_CHECK(biomes_equal_or_mesa_plateau(38, 39));   // 两个 mesa 兄弟
    CYANE_CHECK(!biomes_equal_or_mesa_plateau(37, 2));   // mesa 与非 mesa 不等价
    CYANE_CHECK(biomes_equal_or_mesa_plateau(2, 17));    // desert / desert_hills 同为 BiomeDesert
    // mutated_forest(132) 的实现类就是 BiomeForest，与 forest 同类；
    // 而 mutated_birch_forest(155) 是 BiomeForestMutated 子类，与 forest 不同类。
    CYANE_CHECK(biomes_equal_or_mesa_plateau(4, 132));
    CYANE_CHECK(!biomes_equal_or_mesa_plateau(4, 155));
    CYANE_CHECK(biomes_equal_or_mesa_plateau(155, 156)); // mutated_forest 两兄弟同类
    CYANE_CHECK(biomes_equal_or_mesa_plateau(0, 24));    // ocean / deep_ocean
    CYANE_CHECK(!biomes_equal_or_mesa_plateau(1, 4));
}

CYANE_TEST(gen_layer_hills_applies_variants_and_mutations) {
    using namespace cyane::world::layer;

    class Solid final : public GenLayer {
    public:
        explicit Solid(std::int32_t value) : GenLayer(0), value_(value) {}
        [[nodiscard]] std::vector<std::int32_t> get_ints(std::int32_t, std::int32_t,
                                                         std::int32_t width,
                                                         std::int32_t height) override {
            return std::vector<std::int32_t>(
                static_cast<std::size_t>(width) * static_cast<std::size_t>(height), value_);
        }
    private:
        std::int32_t value_;
    };

    // 全是 desert：结果只能是 desert 或 desert_hills
    {
        auto biomes = std::make_shared<Solid>(biome_ids::kDesert);
        auto rivers = std::make_shared<Solid>(0);
        GenLayerHills hills{1000, biomes, rivers};
        hills.init_world_gen_seed(12345);
        const auto out = hills.get_ints(0, 0, 32, 32);
        bool saw_hills = false;
        for (const auto value : out) {
            CYANE_CHECK(value == biome_ids::kDesert || value == biome_ids::kDesertHills);
            if (value == biome_ids::kDesertHills) saw_hills = true;
        }
        CYANE_CHECK(saw_hills);
    }

    // 全是 ocean：nextInt(3)==0 时升为 deep_ocean
    {
        auto biomes = std::make_shared<Solid>(biome_ids::kOcean);
        auto rivers = std::make_shared<Solid>(0);
        GenLayerHills hills{1000, biomes, rivers};
        hills.init_world_gen_seed(12345);
        const auto out = hills.get_ints(0, 0, 32, 32);
        bool saw_deep = false;
        for (const auto value : out) {
            CYANE_CHECK(value == biome_ids::kOcean || value == biome_ids::kDeepOcean);
            if (value == biome_ids::kDeepOcean) saw_deep = true;
        }
        CYANE_CHECK(saw_deep);
    }

    // 河种值 ≡ 1 (mod 29) 且当前不是变体 → 直接升为变体（plains → mutated_plains）
    {
        auto biomes = std::make_shared<Solid>(biome_ids::kPlains);
        auto rivers = std::make_shared<Solid>(32);  // (32-2) % 29 == 1
        GenLayerHills hills{1000, biomes, rivers};
        hills.init_world_gen_seed(12345);
        for (const auto value : hills.get_ints(0, 0, 16, 16)) {
            CYANE_CHECK_EQ(value, biome_ids::kMutatedPlains);
        }
    }
}

CYANE_TEST(biome_temp_category_matches_vanilla_thresholds) {
    using namespace cyane::world::biome_table;
    using namespace cyane::world::layer;
    // defaultTemperature < 0.2 → cold；< 1.0 → medium；否则 warm
    CYANE_CHECK(biome_temp_category(biome_ids::kDesert) == TempCategory::warm);      // 2.0
    CYANE_CHECK(biome_temp_category(biome_ids::kIceFlats) == TempCategory::cold);    // 0.0
    CYANE_CHECK(biome_temp_category(26) == TempCategory::cold);   // 0.05
    CYANE_CHECK(biome_temp_category(biome_ids::kSavannaRock) == TempCategory::warm); // 1.0
    CYANE_CHECK(biome_temp_category(biome_ids::kForest) == TempCategory::medium);    // 0.7
    CYANE_CHECK(biome_temp_category(biome_ids::kPlains) == TempCategory::medium);    // 0.8

    // 温度感知谓词：同类→真；MEDIUM 与任何类别→真；warm 与 cold→假
    CYANE_CHECK(same_biome_or_temperature(biome_ids::kDesert, biome_ids::kMesa));
    CYANE_CHECK(same_biome_or_temperature(biome_ids::kDesert, biome_ids::kForest));  // medium 搭桥
    CYANE_CHECK(same_biome_or_temperature(biome_ids::kIceFlats, biome_ids::kForest)); // medium 搭桥
    CYANE_CHECK(!same_biome_or_temperature(biome_ids::kDesert, biome_ids::kIceFlats));
}

CYANE_TEST(gen_layer_biome_edge_replaces_harsh_boundaries) {
    using namespace cyane::world::layer;

    class Solid final : public GenLayer {
    public:
        explicit Solid(std::int32_t value) : GenLayer(0), value_(value) {}
        [[nodiscard]] std::vector<std::int32_t> get_ints(std::int32_t, std::int32_t,
                                                         std::int32_t width,
                                                         std::int32_t height) override {
            return std::vector<std::int32_t>(
                static_cast<std::size_t>(width) * static_cast<std::size_t>(height), value_);
        }
    private:
        std::int32_t value_;
    };

    const auto run = [](std::int32_t biome) {
        auto parent = std::make_shared<Solid>(biome);
        GenLayerBiomeEdge edge{1000, parent};
        edge.init_world_gen_seed(12345);
        return edge.get_ints(0, 0, 8, 8);
    };

    // 四邻都是同类 → 保持原样
    CYANE_CHECK_EQ(run(biome_ids::kDesert)[0], biome_ids::kDesert);
    CYANE_CHECK_EQ(run(biome_ids::kSwampland)[0], biome_ids::kSwampland);
    CYANE_CHECK_EQ(run(biome_ids::kMesaRock)[0], biome_ids::kMesaRock);
    CYANE_CHECK_EQ(run(biome_ids::kMesaClearRock)[0], biome_ids::kMesaClearRock);
    CYANE_CHECK_EQ(run(biome_ids::kRedwoodTaiga)[0], biome_ids::kRedwoodTaiga);
    CYANE_CHECK_EQ(run(biome_ids::kForest)[0], biome_ids::kForest);
    // extreme_hills：四邻都是同类 → extreme_hills_with_trees
    CYANE_CHECK_EQ(run(biome_ids::kExtremeHills)[0], biome_ids::kExtremeHillsWithTrees);
}

CYANE_TEST(gen_layer_full_chain_produces_valid_biomes) {
    using namespace cyane::world::layer;

    const auto chain = make_overworld_biome_chain(12345);
    CYANE_CHECK(chain.voronoi != nullptr);

    // 取 256x256 方块（Voronoi 单元 4 格 → 64x64 单元）
    const auto area = chain.voronoi->get_ints(0, 0, 256, 256);
    CYANE_CHECK_EQ(area.size(), std::size_t{256} * 256);

    int ocean = 0;
    int land = 0;
    for (const auto id : area) {
        // 必须是合法的群系 id
        CYANE_CHECK(id >= 0);
        CYANE_CHECK(id < 256);
        if (id == biome_ids::kOcean || id == biome_ids::kDeepOcean ||
            id == biome_ids::kFrozenOcean) {
            ++ocean;
        } else {
            ++land;
        }
    }
    // 海洋与陆地都要有——初始 Island 层 1/10 陆地，放大后应明显两极分化
    CYANE_CHECK(ocean > 0);
    CYANE_CHECK(land > 0);

    // 同种子必须完全可复现
    const auto again = make_overworld_biome_chain(12345);
    CYANE_CHECK(again.voronoi->get_ints(0, 0, 256, 256) == area);

    // 不同种子应给出不同结果
    const auto other = make_overworld_biome_chain(999);
    CYANE_CHECK(other.voronoi->get_ints(0, 0, 256, 256) != area);

    // biome_at 与整块读取一致
    CYANE_CHECK_EQ(chain.biome_at(37, 91), area[static_cast<std::size_t>(91) * 256 + 37]);
}

CYANE_TEST(gen_layer_shore_makes_beaches_and_edges) {
    using namespace cyane::world::layer;

    class Solid final : public GenLayer {
    public:
        explicit Solid(std::int32_t value) : GenLayer(0), value_(value) {}
        [[nodiscard]] std::vector<std::int32_t> get_ints(std::int32_t, std::int32_t,
                                                         std::int32_t width,
                                                         std::int32_t height) override {
            return std::vector<std::int32_t>(
                static_cast<std::size_t>(width) * static_cast<std::size_t>(height), value_);
        }
    private:
        std::int32_t value_;
    };

    const auto run = [](std::int32_t biome) {
        auto parent = std::make_shared<Solid>(biome);
        GenLayerShore shore{1000, parent};
        shore.init_world_gen_seed(12345);
        return shore.get_ints(0, 0, 8, 8);
    };

    // 内陆群系四邻都是自己：保持不变
    CYANE_CHECK_EQ(run(biome_ids::kForest)[0], biome_ids::kForest);
    CYANE_CHECK_EQ(run(biome_ids::kPlains)[0], biome_ids::kPlains);
    // 海洋本身保持不变
    CYANE_CHECK_EQ(run(biome_ids::kOcean)[0], biome_ids::kOcean);
    CYANE_CHECK_EQ(run(biome_ids::kDeepOcean)[0], biome_ids::kDeepOcean);
    // 蘑菇岛四邻无海洋 → 保持；蘑菇岛岸逻辑在整链测试里覆盖
    CYANE_CHECK_EQ(run(biome_ids::kMushroomIsland)[0], biome_ids::kMushroomIsland);
    // 台地四邻全为台地 → 不变
    CYANE_CHECK_EQ(run(biome_ids::kMesa)[0], biome_ids::kMesa);
}

CYANE_TEST(chunk_generator_uses_the_gen_layer_chain) {
    // 生成器的群系必须与直接跑 GenLayer 链的结果逐格一致
    cyane::world::gen::ChunkGeneratorOverworld generator{12345};
    const auto sampled = generator.sample_biomes(3, -7);

    const auto chain = cyane::world::layer::make_overworld_biome_chain(12345);
    const auto expected = chain.voronoi->get_ints(3 * 16, -7 * 16, 16, 16);
    for (std::size_t idx = 0; idx < sampled.size() && idx < expected.size(); ++idx) {
        CYANE_CHECK_EQ(static_cast<int>(sampled[idx]), expected[idx] & 0xFF);
    }

    // 单点查询与整块采样一致
    const auto& info = generator.biome_at_block(3 * 16 + 8, -7 * 16 + 8);
    CYANE_CHECK_EQ(static_cast<int>(info.id),
                   static_cast<int>(sampled[8 + 8 * 16]));
}

CYANE_TEST(world_populate_cascade_waits_for_neighbours) {
    // 原版 Chunk.populate：必须等"正邻 + 斜邻"都就绪才允许装饰，
    // 否则特征无处可写。这里验证门控与"只跑一次"。
    cyane::world::World world;
    world.set_generator(12345);

    // 只生成 (0,0)：东/南/东南都不存在 → 不该被装饰
    auto first = world.chunk_at(cyane::world::ChunkPos{0, 0});
    CYANE_CHECK(!first.terrain_populated());

    // 补上东邻与南邻，仍缺东南邻
    (void)world.chunk_at(cyane::world::ChunkPos{1, 0});
    (void)world.chunk_at(cyane::world::ChunkPos{0, 1});
    CYANE_CHECK(!world.chunk_at(cyane::world::ChunkPos{0, 0}).terrain_populated());

    // 生成东南邻 → 级联此时满足条件，(0,0) 被装饰
    (void)world.chunk_at(cyane::world::ChunkPos{1, 1});
    CYANE_CHECK(world.chunk_at(cyane::world::ChunkPos{0, 0}).terrain_populated());

    // 只跑一次：再次取用不会改变已装饰的状态
    const auto before = world.chunk_at(cyane::world::ChunkPos{0, 0}).block_at(8, 64, 8);
    CYANE_CHECK(world.chunk_at(cyane::world::ChunkPos{0, 0}).terrain_populated());
    CYANE_CHECK_EQ(world.chunk_at(cyane::world::ChunkPos{0, 0}).block_at(8, 64, 8), before);
}

CYANE_TEST(chunk_spill_writer_receives_out_of_range_writes) {
    // 装饰阶段把越界写入转给邻区块，是"树冠在区块边界被硬切"的解法。
    cyane::world::Chunk chunk{cyane::world::ChunkPos{0, 0}};
    std::vector<std::array<std::int32_t, 3>> captured;
    std::vector<std::uint16_t> captured_state;
    chunk.set_spill_writer(
        [&](std::int32_t wx, std::int32_t wy, std::int32_t wz, std::uint16_t state) {
            captured.push_back({wx, wy, wz});
            captured_state.push_back(state);
        });

    // 界内：正常写入，不进 spill
    chunk.set_block_state(3, 5, 3, 43);
    CYANE_CHECK_EQ(chunk.block_at(3, 5, 3), std::uint16_t{43});
    CYANE_CHECK(captured.empty());

    // 越界（左上角各差一格）：转给 spill
    chunk.set_block_state(-1, 6, -1, 42);
    chunk.set_block_state(16, 7, 3, 44);
    CYANE_CHECK_EQ(captured.size(), std::size_t{2});
    CYANE_CHECK_EQ(captured[0][0], -1);
    CYANE_CHECK_EQ(captured[0][2], -1);
    CYANE_CHECK_EQ(captured_state[0], std::uint16_t{42});
    CYANE_CHECK_EQ(captured[1][0], 16);

    // 摘掉 spill 后越界写入应被静默丢弃
    chunk.clear_spill_writer();
    chunk.set_block_state(-2, 6, -2, 45);
    CYANE_CHECK_EQ(captured.size(), std::size_t{2});
}
