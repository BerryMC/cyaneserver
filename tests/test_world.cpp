#include <cstring>
#include <filesystem>
#include <fstream>

#include "cyane/game/world_persistence.hpp"
#include "cyane/net/container_store.hpp"
#include "cyane/net/furnace_store.hpp"
#include "cyane/net/item_drop.hpp"
#include "cyane/proto/frame.hpp"
#include "cyane/world/level_dat.hpp"
#include "cyane/world/nbt.hpp"
#include "cyane/world/anvil.hpp"
#include "cyane/world/world.hpp"
#include "test_framework.hpp"

using namespace cyane;
namespace nbt = cyane::world::nbt;

namespace {

[[nodiscard]] std::filesystem::path fixture_path(const char* name) {
    for (const std::string& candidate :
         {std::string{name}, std::string{"tests/fixtures/"} + name,
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
    CYANE_CHECK(!decoded->chunk.sections().empty());
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
    CYANE_CHECK_EQ(info->spawn_y, 4);
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
    for (std::size_t sy = 0; sy < original->chunk.sections().size() && probe_y < 0; ++sy) {
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
        nbt::Compound zombie_fields;
        zombie_fields.emplace_back("Pos", nbt::make_list(nbt::List{
            nbt::Value{nbt::Tag::f64, 100.0},
            nbt::Value{nbt::Tag::f64, 5.0},
            nbt::Value{nbt::Tag::f64, 100.0}}));
        zombie_fields.emplace_back("id",
                                   nbt::Value{nbt::Tag::string, std::string{"minecraft:zombie"}});
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
        entity_list.push_back(nbt::make_compound(std::move(zombie_fields)));
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
        bool zombie_kept = false;
        bool pig_rewritten = false;
        for (const auto& entry : *ents) {
            const auto id = entry.find("id") ? entry.find("id")->text() : std::nullopt;
            if (id && *id == "minecraft:zombie") {
                zombie_kept = true;
            }
            if (id && *id == "minecraft:cow") {
                pig_rewritten = true;
            }
        }
        CYANE_CHECK(zombie_kept);
        CYANE_CHECK(pig_rewritten);
        CYANE_CHECK_EQ(ents->size(), std::size_t{3});  // 僵尸 + 牛 + 内存态物品
    }
}
