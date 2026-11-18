#include <array>
#include <bit>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <type_traits>

#include "cyane/core/bytes.hpp"
#include "cyane/game/server.hpp"
#include "cyane/world/region.hpp"
#include "cyane/world/save_format.hpp"
#include "cyane/world/save_handler.hpp"
#include "test_framework.hpp"

namespace {

class TemporaryWorld {
public:
    TemporaryWorld() {
        auto pattern = (std::filesystem::temp_directory_path() / "cyane_session_XXXXXX").string();
        if (const auto* created = ::mkdtemp(pattern.data()); created != nullptr) path = created;
        else throw std::runtime_error("cannot create isolated test world");
    }
    ~TemporaryWorld() { std::error_code ec; std::filesystem::remove_all(path, ec); }
    std::filesystem::path path;
};

std::string read_bytes(const std::filesystem::path& path) {
    std::ifstream in{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

void write_bytes(const std::filesystem::path& path, std::string_view bytes) {
    std::ofstream out{path, std::ios::binary | std::ios::trunc};
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

cyane::ServerConfig server_config(const TemporaryWorld& world) {
    cyane::ServerConfig config;
    config.world_dir = world.path.string();
    config.player_data_dir = (world.path / "playerdata").string();
    config.op_file = (world.path / "ops.json").string();
    config.recipe_file = (world.path / "missing_recipes.toml").string();
    config.bind_address = "127.0.0.1";
    config.port = 0;
    config.worker_threads = 1;
    config.seed = "42";
    return config;
}

}  // namespace

CYANE_TEST(session_lock_uses_java_big_endian_long) {
    TemporaryWorld dir;
    cyane::world::storage::SaveHandler handler;
    constexpr std::int64_t stamp = 0x0102030405060708LL;
    CYANE_CHECK(handler.open(dir.path, stamp).has_value());
    const std::string expected{"\x01\x02\x03\x04\x05\x06\x07\x08", 8};
    CYANE_CHECK_EQ(read_bytes(handler.session_lock_path()), expected);
    CYANE_CHECK(handler.check_session_lock().has_value());
    CYANE_CHECK_EQ(handler.initialization_time(), stamp);
    CYANE_CHECK_EQ(handler.map_file("map_7"), dir.path / "data" / "map_7.dat");
    static_assert(!std::is_copy_constructible_v<cyane::world::storage::SaveHandler>);
}

CYANE_TEST(session_lock_accepts_zero_and_negative_timestamps) {
    TemporaryWorld dir;
    for (const auto stamp : {std::int64_t{0}, std::int64_t{-1}, std::int64_t{-123456789}}) {
        cyane::world::storage::SaveHandler handler;
        CYANE_CHECK(handler.open(dir.path, stamp).has_value());
        CYANE_CHECK_EQ(handler.initialization_time(), stamp);
        CYANE_CHECK(handler.check_session_lock().has_value());
    }
}

CYANE_TEST(session_lock_takeover_invalidates_only_old_owner) {
    TemporaryWorld dir;
    cyane::world::storage::SaveHandler first, second;
    CYANE_CHECK(first.open(dir.path, 101).has_value());
    CYANE_CHECK(second.open(dir.path, 202).has_value());
    const auto check = first.check_session_lock();
    CYANE_CHECK(!check.has_value());
    if (!check) CYANE_CHECK_EQ(check.error().message,
        "The save is being accessed from another location, aborting");
    CYANE_CHECK(second.check_session_lock().has_value());
    const auto bytes = read_bytes(second.session_lock_path());
    first.close();
    CYANE_CHECK_EQ(read_bytes(second.session_lock_path()), bytes);
    second.close();
    CYANE_CHECK_EQ(read_bytes(second.session_lock_path()), bytes);
}

CYANE_TEST(session_lock_retains_file_after_destruction) {
    TemporaryWorld dir;
    {
        cyane::world::storage::SaveHandler handler;
        CYANE_CHECK(handler.open(dir.path, 11).has_value());
    }
    CYANE_CHECK(std::filesystem::exists(dir.path / "session.lock"));
    cyane::world::storage::SaveHandler next;
    CYANE_CHECK(next.open(dir.path, 22).has_value());
    CYANE_CHECK(next.check_session_lock().has_value());
    CYANE_CHECK(!next.open(dir.path, 33).has_value());
    CYANE_CHECK(next.check_session_lock().has_value());
}

CYANE_TEST(session_lock_reports_missing_truncated_and_wrong_endian_files) {
    TemporaryWorld dir;
    cyane::world::storage::SaveHandler handler;
    CYANE_CHECK(handler.open(dir.path, 0x0102030405060708LL).has_value());
    const auto lock = handler.session_lock_path();
    std::filesystem::remove(lock);
    CYANE_CHECK(!handler.check_session_lock().has_value());
    write_bytes(lock, "short");
    const auto truncated = handler.check_session_lock();
    CYANE_CHECK(!truncated.has_value());
    if (!truncated) CYANE_CHECK_EQ(truncated.error().message, "Failed to check session lock, aborting");
    write_bytes(lock, std::string{"\x08\x07\x06\x05\x04\x03\x02\x01", 8});
    CYANE_CHECK(!handler.check_session_lock().has_value());
    // Java readLong 只读取前八字节，不拒绝多余尾部。
    write_bytes(lock, std::string{"\x01\x02\x03\x04\x05\x06\x07\x08suffix", 14});
    CYANE_CHECK(handler.check_session_lock().has_value());
}

CYANE_TEST(session_lock_creation_failure_aborts_server_before_world_writes) {
    TemporaryWorld dir;
    std::filesystem::create_directory(dir.path / "session.lock");
    const auto config = server_config(dir);
    const auto server = cyane::Server::create(config);
    CYANE_CHECK(!server.has_value());
    CYANE_CHECK(!std::filesystem::exists(dir.path / "level.dat"));
    CYANE_CHECK(!std::filesystem::exists(dir.path / "region"));
    CYANE_CHECK(!std::filesystem::exists(dir.path / "ops.json"));
}

CYANE_TEST(save_format_old_manages_vanilla_world_directories) {
    TemporaryWorld root;
    cyane::world::storage::SaveFormatOld format{root.path / "saves"};
    CYANE_CHECK_EQ(format.name(), "Old Format");
    CYANE_CHECK(format.is_new_level_id_acceptable("World1"));
    CYANE_CHECK(!std::filesystem::exists(root.path / "saves" / "World1"));

    auto loader = format.get_save_loader("World1", false);
    CYANE_CHECK(loader.has_value());
    if (!loader) return;
    const auto world_dir = root.path / "saves" / "World1";
    CYANE_CHECK(std::filesystem::exists(world_dir / "data"));
    CYANE_CHECK(!std::filesystem::exists(world_dir / "playerdata"));
    CYANE_CHECK(format.can_load_world("World1"));
    CYANE_CHECK_EQ(format.get_file("World1", "level.dat"), world_dir / "level.dat");

    cyane::world::storage::WorldInfo info;
    info.level_name = "Before Rename";
    info.random_seed = 1234;
    CYANE_CHECK((*loader)->save_world_info(info).has_value());
    const auto lock_before = read_bytes(world_dir / "session.lock");

    auto loaded = format.get_world_info("World1");
    CYANE_CHECK(loaded.has_value() && loaded->has_value());
    if (loaded && loaded->has_value()) {
        CYANE_CHECK_EQ((**loaded).level_name, "Before Rename");
        CYANE_CHECK_EQ((**loaded).random_seed, 1234);
    }
    const auto summaries = format.get_save_list();
    CYANE_CHECK(summaries.has_value());
    if (summaries) {
        CYANE_CHECK_EQ(summaries->size(), 1ULL);
        CYANE_CHECK_EQ((*summaries)[0].level_name, "Before Rename");
    }

    CYANE_CHECK(format.rename_world("World1", "After Rename").has_value());
    CYANE_CHECK_EQ(read_bytes(world_dir / "session.lock"), lock_before);
    loaded = format.get_world_info("World1");
    CYANE_CHECK(loaded.has_value() && loaded->has_value());
    if (loaded && loaded->has_value()) CYANE_CHECK_EQ((**loaded).level_name, "After Rename");

    CYANE_CHECK(format.delete_world_directory("World1"));
    CYANE_CHECK(!format.can_load_world("World1"));
    CYANE_CHECK(format.delete_world_directory("World1"));
}

CYANE_TEST(save_handler_rotates_level_dat_and_preserves_unknown_fields) {
    TemporaryWorld dir;
    cyane::world::storage::SaveHandler handler;
    CYANE_CHECK(handler.open(dir.path, 333).has_value());

    cyane::world::nbt::Compound data;
    data.emplace_back("RandomSeed", cyane::world::nbt::make_i64(10));
    data.emplace_back("LevelName", cyane::world::nbt::make_string("SpigotWorld"));
    data.emplace_back("Bukkit.Version", cyane::world::nbt::make_string("git-Spigot-compat"));
    cyane::world::nbt::Compound root;
    root.emplace_back("Data", cyane::world::nbt::make_compound(std::move(data)));
    cyane::world::storage::WorldInfo info;
    info.read_root(cyane::world::nbt::make_compound(std::move(root)));
    info.random_seed = 111;
    info.world_time = 50;
    info.border_center_x = 12.5;
    info.border_warning_distance = 9;
    CYANE_CHECK(handler.save_world_info(info).has_value());
    CYANE_CHECK(std::filesystem::exists(handler.level_dat()));
    CYANE_CHECK(!std::filesystem::exists(dir.path / "level.dat_new"));
    CYANE_CHECK(!std::filesystem::exists(handler.level_dat_old()));

    const auto first = handler.load_world_info();
    CYANE_CHECK(first.has_value());
    if (!first) return;
    CYANE_CHECK_EQ(first->random_seed, 111);
    CYANE_CHECK_EQ(first->world_time, 50);
    CYANE_CHECK_EQ(first->border_center_x, 12.5);
    CYANE_CHECK_EQ(first->border_warning_distance, 9);
    // 未建模的 Bukkit 键必须原样保留。
    const auto first_bytes = read_bytes(handler.level_dat());
    const auto parsed = cyane::world::nbt::parse_compressed(cyane::as_bytes(first_bytes));
    CYANE_CHECK(parsed.has_value());
    const auto* first_data = parsed->find("Data");
    CYANE_CHECK(first_data != nullptr);
    if (first_data != nullptr) {
        const auto* bukkit = first_data->find("Bukkit.Version");
        CYANE_CHECK(bukkit != nullptr);
        if (bukkit != nullptr) {
            CYANE_CHECK_EQ(bukkit->text().value_or(""), "git-Spigot-compat");
        }
        CYANE_CHECK(first_data->find("Border") == nullptr); // 边界字段位于 Data 根，不是子 compound。
        const auto* center = first_data->find("BorderCenterX");
        CYANE_CHECK(center != nullptr && center->is(cyane::world::nbt::Tag::f64));
        const auto* warning = first_data->find("BorderWarningBlocks");
        CYANE_CHECK(warning != nullptr && warning->is(cyane::world::nbt::Tag::f64));
    }

    // 第二次保存：旧 current 原子改名成 level.dat_old，新的变 current。
    auto second = *first;
    second.random_seed = 222;
    second.world_time = 99;
    CYANE_CHECK(handler.save_world_info(second).has_value());
    CYANE_CHECK(std::filesystem::exists(handler.level_dat_old()));
    CYANE_CHECK(!std::filesystem::exists(dir.path / "level.dat_new"));
    const auto current = handler.load_world_info();
    CYANE_CHECK(current.has_value());
    if (current) CYANE_CHECK_EQ(current->random_seed, 222);

    // 直接解析备份，必须仍是第一次保存内容。
    const auto old_bytes = read_bytes(handler.level_dat_old());
    const auto old_root = cyane::world::nbt::parse_compressed(cyane::as_bytes(old_bytes));
    CYANE_CHECK(old_root.has_value());
    if (old_root) {
        cyane::world::storage::WorldInfo old;
        old.read_root(*old_root);
        CYANE_CHECK_EQ(old.random_seed, 111);
        CYANE_CHECK_EQ(old.world_time, 50);
    }
}

CYANE_TEST(save_handler_falls_back_to_level_dat_old) {
    TemporaryWorld dir;
    cyane::world::storage::SaveHandler handler;
    CYANE_CHECK(handler.open(dir.path, 444).has_value());
    cyane::world::storage::WorldInfo first;
    first.random_seed = 7;
    CYANE_CHECK(handler.save_world_info(first).has_value());
    first.random_seed = 8;
    CYANE_CHECK(handler.save_world_info(first).has_value());
    // 破坏 current；loadWorldInfo 对齐 SaveHandler：改读 level.dat_old。
    write_bytes(handler.level_dat(), "broken");
    const auto fallback = handler.load_world_info();
    CYANE_CHECK(fallback.has_value());
    if (fallback) CYANE_CHECK_EQ(fallback->random_seed, 7);
}

CYANE_TEST(save_handler_forces_anvil_save_version) {
    TemporaryWorld dir;
    cyane::world::storage::SaveHandler handler;
    CYANE_CHECK(handler.open(dir.path, 777).has_value());
    // 模拟从旧档载入的 version=0；AnvilSaveHandler 每次保存都会改成 19133
    cyane::world::storage::WorldInfo info;
    info.save_version = 0;
    CYANE_CHECK(handler.save_world_info(info).has_value());
    const auto reloaded = handler.load_world_info();
    CYANE_CHECK(reloaded.has_value());
    if (reloaded) CYANE_CHECK_EQ(reloaded->save_version, 19133);
}

CYANE_TEST(level_dat_save_is_rejected_after_takeover) {
    TemporaryWorld dir;
    cyane::world::storage::SaveHandler old_owner, new_owner;
    CYANE_CHECK(old_owner.open(dir.path, 1001).has_value());
    cyane::world::storage::WorldInfo info;
    info.random_seed = 1;
    CYANE_CHECK(old_owner.save_world_info(info).has_value());
    const auto before = read_bytes(old_owner.level_dat());
    CYANE_CHECK(new_owner.open(dir.path, 1002).has_value());
    info.random_seed = 2;
    CYANE_CHECK(!old_owner.save_world_info(info).has_value());
    CYANE_CHECK_EQ(read_bytes(old_owner.level_dat()), before);
    CYANE_CHECK(new_owner.save_world_info(info).has_value());
    const auto loaded = new_owner.load_world_info();
    CYANE_CHECK(loaded.has_value());
    if (loaded) CYANE_CHECK_EQ(loaded->random_seed, 2);
}

CYANE_TEST(server_persists_time_to_level_dat_on_shutdown) {
    TemporaryWorld dir;
    auto server = cyane::Server::create(server_config(dir));
    CYANE_CHECK(server.has_value());
    if (!server) return;
    CYANE_CHECK_EQ((*server)->run(25), 0);
    server->reset(); // 析构不删除原版 session.lock。

    cyane::world::storage::SaveHandler reader;
    CYANE_CHECK(reader.open(dir.path, 888).has_value());
    const auto info = reader.load_world_info();
    CYANE_CHECK(info.has_value());
    if (info) {
        CYANE_CHECK_EQ(info->world_time, 25);
        CYANE_CHECK_EQ(info->day_time, 25);
        CYANE_CHECK_EQ(info->random_seed, 42);
    }
}

CYANE_TEST(server_world_save_is_rejected_after_session_takeover) {
    TemporaryWorld dir;
    auto server = cyane::Server::create(server_config(dir));
    CYANE_CHECK(server.has_value());
    if (!server) return;
    auto* world = (*server)->world();
    world->set_block(0, 100, 0, cyane::world::kStateStone);
    (*server)->save_world_now();
    const auto region = cyane::world::RegionFile::path_for((dir.path / "region").string(), 0, 0);
    const auto before = read_bytes(region);
    CYANE_CHECK(!before.empty());

    const auto lock = read_bytes(dir.path / "session.lock");
    cyane::ByteReader reader{cyane::as_bytes(lock)};
    const auto stamp = reader.i64();
    CYANE_CHECK(stamp.has_value());
    if (!stamp) return;
    cyane::world::storage::SaveHandler replacement;
    CYANE_CHECK(replacement.open(dir.path, *stamp ^ 1).has_value());
    world->set_block(0, 100, 0, cyane::world::kStateDirt);
    (*server)->save_world_now();
    CYANE_CHECK_EQ(read_bytes(region), before);
    CYANE_CHECK(!world->dirty_chunks().empty());
    // 模拟另一个进程恢复原时间戳后，旧实例又能保存；不引入 OS 独占锁。
    write_bytes(dir.path / "session.lock", lock);
    (*server)->save_world_now();
    CYANE_CHECK(read_bytes(region) != before);
}
