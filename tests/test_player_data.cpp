#include <filesystem>
#include <fstream>

#include "cyane/game/player_data.hpp"
#include "cyane/world/nbt.hpp"
#include "test_framework.hpp"

using namespace cyane;
namespace nbt = cyane::world::nbt;

namespace {

constexpr std::int16_t kApple = 260;
constexpr std::int16_t kDiamondChestplate = 311;

[[nodiscard]] std::filesystem::path fixture_path() {
    for (const auto* candidate : {"vanilla_player_oracle.dat",
                                  "tests/fixtures/vanilla_player_oracle.dat",
                                  "../tests/fixtures/vanilla_player_oracle.dat"}) {
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

[[nodiscard]] std::string dump_temp_dir() {
    return (std::filesystem::temp_directory_path() / "cyane_test_playerdata").string();
}

} // namespace

CYANE_TEST(vanilla_oracle_player_dat_parses) {
    const auto path = fixture_path();
    CYANE_CHECK(!path.empty());
    if (path.empty()) {
        return;
    }
    const auto raw = read_file(path);
    CYANE_CHECK(raw.size() > 2);
    // 原版 .dat 是 gzip 容器
    CYANE_CHECK_EQ(static_cast<std::uint8_t>(raw[0]), std::uint8_t{0x1F});
    CYANE_CHECK_EQ(static_cast<std::uint8_t>(raw[1]), std::uint8_t{0x8B});

    const auto root = nbt::parse_compressed(
        ByteSpan{reinterpret_cast<const std::byte*>(raw.data()), raw.size()});
    CYANE_CHECK(root.has_value());
    // oracle 实测：根 compound 直接包含字段，无 "Data" 包装
    CYANE_CHECK_EQ(root->find("DataVersion")->scalar().value_or(0), 1343);
    CYANE_CHECK_EQ(root->find("playerGameType")->scalar().value_or(-1), 0);
    const auto* health = root->find("Health");
    CYANE_CHECK(health != nullptr);
    const auto* health_value = health->get_if<float>();
    CYANE_CHECK(health_value != nullptr && *health_value == 20.0f);

    const auto* pos = root->find("Pos");
    const auto* pos_list = pos != nullptr ? pos->get_if<nbt::List>() : nullptr;
    CYANE_CHECK(pos_list != nullptr && pos_list->size() == 3);
    const auto* pos_x = (*pos_list)[0].get_if<double>();
    CYANE_CHECK(pos_x != nullptr && *pos_x == 999.5);

    const auto* inv = root->find("Inventory");
    const auto* inv_list = inv != nullptr ? inv->get_if<nbt::List>() : nullptr;
    CYANE_CHECK(inv_list != nullptr && inv_list->empty());
    CYANE_CHECK(root->find("UUIDMost") != nullptr);
    CYANE_CHECK(root->find("abilities")->find("flySpeed") != nullptr);
}

CYANE_TEST(player_dat_round_trip_with_slot_mapping) {
    const auto dir = dump_temp_dir();
    std::filesystem::remove_all(dir);
    game::PlayerDataStore store;
    store.set_dir(dir);

    game::PlayerData data;
    data.uuid_with_dashes = "b50ad385-829d-3141-a216-7e7d7539ba7f";
    data.username = "NbtProbe";
    data.x = 12.5;
    data.y = 6.0;
    data.z = -7.25;
    data.yaw = 90.0f;
    data.pitch = -15.5f;
    data.game_mode = proto::game_mode::kSurvival;
    data.health = 13.5f;
    data.selected_slot = 4;
    data.inventory[36] = item::ItemStack{kApple, 5, 0};   // 热区栏 0 → NBT Slot 0
    data.inventory[40] = item::ItemStack{kDiamondChestplate, 1, 0};  // 热区栏 4 → NBT Slot 4
    data.inventory[10] = item::ItemStack{1, 64, 0};       // 主背包 → NBT Slot 10
    data.inventory[5] = item::ItemStack{kDiamondChestplate, 1, 0};   // 头 → NBT Slot 103
    data.inventory[8] = item::ItemStack{301, 1, 0};       // 脚 → NBT Slot 100
    data.inventory[45] = item::ItemStack{kApple, 1, 0};   // 副手 → NBT Slot 40
    data.inventory[2] = item::ItemStack{1, 1, 0};         // 合成格 → 原版不持久化

    CYANE_CHECK(store.save(data).has_value());
    const auto dat_path = std::filesystem::path{dir} / (data.uuid_with_dashes + ".dat");
    CYANE_CHECK(std::filesystem::exists(dat_path));
    const auto raw = read_file(dat_path);
    CYANE_CHECK_EQ(static_cast<std::uint8_t>(raw[0]), std::uint8_t{0x1F});
    CYANE_CHECK_EQ(static_cast<std::uint8_t>(raw[1]), std::uint8_t{0x8B});

    // 直接解析 NBT 验证原版槽位编号与字段名
    const auto root = nbt::parse_compressed(
        ByteSpan{reinterpret_cast<const std::byte*>(raw.data()), raw.size()});
    CYANE_CHECK(root.has_value());
    const auto* inv = root->find("Inventory");
    const auto* items = inv != nullptr ? inv->get_if<nbt::List>() : nullptr;
    CYANE_CHECK(items != nullptr && items->size() == 6);
    for (const auto& entry : *items) {
        const auto slot = entry.find("Slot")->scalar().value_or(-1);
        const auto id = entry.find("id")->scalar().value_or(-1);
        const auto expected = [&] {
            switch (slot) {
                case 0: return 260;
                case 4: return 311;
                case 10: return 1;
                case 103: return 311;
                case 100: return 301;
                case 40: return 260;
                default: return -1;
            }
        }();
        CYANE_CHECK_EQ(id, expected);
    }

    // 读回：全字段还原
    const auto loaded = store.load_or_default(data.uuid_with_dashes, "NbtProbe", proto::game_mode::kSurvival, 0.5, 4.0, 0.5);
    CYANE_CHECK_EQ(loaded.x, 12.5);
    CYANE_CHECK_EQ(loaded.y, 6.0);
    CYANE_CHECK_EQ(loaded.z, -7.25);
    CYANE_CHECK_EQ(loaded.yaw, 90.0f);
    CYANE_CHECK_EQ(loaded.pitch, -15.5f);
    CYANE_CHECK_EQ(static_cast<int>(loaded.game_mode), static_cast<int>(proto::game_mode::kSurvival));
    CYANE_CHECK_EQ(loaded.health, 13.5f);
    CYANE_CHECK_EQ(loaded.selected_slot, std::uint8_t{4});
    CYANE_CHECK_EQ(loaded.inventory[36].id, kApple);
    CYANE_CHECK_EQ(loaded.inventory[36].count, std::uint8_t{5});
    CYANE_CHECK_EQ(loaded.inventory[40].id, kDiamondChestplate);
    CYANE_CHECK_EQ(loaded.inventory[10].id, std::int16_t{1});
    CYANE_CHECK_EQ(loaded.inventory[10].damage, std::int16_t{0});
    CYANE_CHECK_EQ(loaded.inventory[8].id, std::int16_t{301});
    CYANE_CHECK_EQ(loaded.inventory[45].id, kApple);
    CYANE_CHECK(loaded.inventory[2].empty());   // 合成格不持久化
    CYANE_CHECK(loaded.inventory[11].empty());

    std::filesystem::remove_all(dir);
}

CYANE_TEST(player_dat_reads_vanilla_written_file) {
    const auto path = fixture_path();
    CYANE_CHECK(!path.empty());
    if (path.empty()) {
        return;
    }
    const auto dir = dump_temp_dir();
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    std::filesystem::copy_file(path,
                               std::filesystem::path{dir} / "b50ad385-829d-3141-a216-7e7d7539ba7f.dat",
                               std::filesystem::copy_options::overwrite_existing);

    game::PlayerDataStore store;
    store.set_dir(dir);
    const auto data = store.load_or_default("b50ad385-829d-3141-a216-7e7d7539ba7f", "OracleProbe", proto::game_mode::kSurvival, 0.5, 4.0, 0.5);
    // oracle 文件实测值：出生超平坦 (999.5, 4, 663.5)、生存、满血
    CYANE_CHECK_EQ(data.x, 999.5);
    CYANE_CHECK_EQ(data.y, 4.0);
    CYANE_CHECK_EQ(data.z, 663.5);
    CYANE_CHECK_EQ(static_cast<int>(data.game_mode), 0);
    CYANE_CHECK_EQ(data.health, 20.0f);
    CYANE_CHECK(data.inventory[36].empty());

    std::filesystem::remove_all(dir);
}

CYANE_TEST(legacy_json_migrates_to_dat) {
    const auto dir = dump_temp_dir();
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const std::string uuid = "b50ad385-829d-3141-a216-7e7d7539ba7f";
    {
        std::ofstream out{std::filesystem::path{dir} / (uuid + ".json"), std::ios::trunc};
        out << "{\n"
            << "  \"username\": \"OldProbe\",\n"
            << "  \"uuid\": \"" << uuid << "\",\n"
            << "  \"x\": 3.500000,\n"
            << "  \"y\": 4.000000,\n"
            << "  \"z\": 5.500000,\n"
            << "  \"yaw\": 0.000000,\n"
            << "  \"pitch\": 0.000000,\n"
            << "  \"game_mode\": 1,\n"
            << "  \"health\": 18.000000,\n"
            << "  \"inventory\": [\n"
            << "    {\"slot\": 36, \"id\": 260, \"count\": 3, \"damage\": 0}\n"
            << "  ]\n"
            << "}";
    }

    game::PlayerDataStore store;
    store.set_dir(dir);
    const auto data = store.load_or_default(uuid, "OldProbe", proto::game_mode::kSurvival, 0.5, 4.0, 0.5);
    CYANE_CHECK_EQ(data.x, 3.5);
    CYANE_CHECK_EQ(data.z, 5.5);
    CYANE_CHECK_EQ(data.health, 18.0f);
    CYANE_CHECK_EQ(static_cast<int>(data.game_mode), 1);
    CYANE_CHECK_EQ(data.inventory[36].id, kApple);
    CYANE_CHECK_EQ(data.inventory[36].count, std::uint8_t{3});
    // 迁移后 .dat 就位、.json 移除
    CYANE_CHECK(std::filesystem::exists(std::filesystem::path{dir} / (uuid + ".dat")));
    CYANE_CHECK(!std::filesystem::exists(std::filesystem::path{dir} / (uuid + ".json")));

    std::filesystem::remove_all(dir);
}
CYANE_TEST(new_player_defaults_to_world_spawn_not_origin) {
    const auto tmp = std::filesystem::temp_directory_path() / "cyane_test_newspawn";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);
    game::PlayerDataStore store;
    store.set_dir(tmp.string());
    // 无 .dat 的新玩家：位置应为传入的世界出生点，而非硬编码原点
    const auto data = store.load_or_default("00000000-0000-0000-0000-000000000001", "Newbie",
                                            proto::game_mode::kSurvival, -28.5, 72.0, 244.5);
    CYANE_CHECK(data.x == -28.5);
    CYANE_CHECK(data.y == 72.0);
    CYANE_CHECK(data.z == 244.5);
    std::filesystem::remove_all(tmp);
}
