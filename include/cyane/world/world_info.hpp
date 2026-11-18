#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "cyane/world/difficulty.hpp"
#include "cyane/world/game_rules.hpp"
#include "cyane/world/nbt.hpp"
#include "cyane/world/world_settings.hpp"

namespace cyane::world::storage {

// 1.12.2 原版世界元数据（WorldInfo）。读取时保留整个 Data compound，写回只覆盖
// 原版已建模字段，因此 Bukkit/Spigot/Forge 添加的未知字段不会丢失。
class WorldInfo {
public:
    WorldInfo()
        : WorldInfo{WorldSettings{0, 0, true, false, WorldType::Default}} {}

    explicit WorldInfo(const WorldSettings& settings, std::string name = "world")
        : level_name{std::move(name)},
          generator_options{settings.generator_options()},
          terrain_type{settings.terrain_type()},
          map_features_enabled{settings.map_features_enabled()},
          allow_commands{settings.commands_allowed()},
          hardcore{settings.hardcore_enabled()},
          game_type{settings.game_type()},
          random_seed{settings.seed()} {}

    std::int32_t version_id{1343};
    std::string version_name{"1.12.2"};
    bool version_snapshot{false};
    bool initialized{false};

    std::string level_name{"world"};
    std::string generator_options;
    WorldType terrain_type{WorldType::Default};
    bool map_features_enabled{true};
    bool allow_commands{false};
    bool hardcore{false};

    std::uint8_t game_type{0};
    EnumDifficulty difficulty{EnumDifficulty::Normal};
    bool difficulty_locked{false};
    GameRules game_rules{};
    std::int64_t random_seed{0};
    std::int64_t world_time{0};
    std::int64_t day_time{0};
    std::int64_t last_played{0};
    std::int64_t size_on_disk{0};
    std::int32_t save_version{19133};

    std::int32_t spawn_x{0};
    std::int32_t spawn_y{64};
    std::int32_t spawn_z{0};

    bool raining{false};
    std::int32_t rain_time{0};
    bool thundering{false};
    std::int32_t thunder_time{0};
    std::int32_t clear_weather_time{0};

    double border_center_x{0.0};
    double border_center_z{0.0};
    double border_diameter{6.0E7};
    std::int64_t border_size_lerp_time{0};
    double border_size_lerp_target{0.0};
    double border_safe_zone{5.0};
    double border_damage_amount{0.2};
    std::int32_t border_warning_time{15};
    std::int32_t border_warning_distance{5};

    [[nodiscard]] std::uint8_t game_mode() const noexcept { return game_type; }
    void set_game_mode(std::uint8_t mode) noexcept { game_type = mode; }

    void read_from_nbt(const nbt::Value& data) {
        if (!data.is(nbt::Tag::compound)) return;
        source_data_ = data;
        if (const auto* version = data.find("Version"); version != nullptr) {
            version_id = static_cast<std::int32_t>(int_field(*version, "Id", version_id));
            version_name = string_field(*version, "Name", version_name);
            version_snapshot = bool_field(*version, "Snapshot", version_snapshot);
        }
        random_seed = int_field(data, "RandomSeed", random_seed);
        generator_options = string_field(data, "generatorOptions", generator_options);
        const auto generator_name = string_field(data, "generatorName", "default");
        const auto generator_version = static_cast<std::int32_t>(int_field(data, "generatorVersion", 0));
        if (const auto type = world_type_by_name(generator_name); type.has_value()) {
            terrain_type = *type == WorldType::Default && generator_version == 0
                ? WorldType::Default1_1 : *type;
        } else {
            terrain_type = WorldType::Default;
        }
        game_type = static_cast<std::uint8_t>(int_field(data, "GameType", game_type));
        map_features_enabled = bool_field(data, "MapFeatures", true);
        spawn_x = static_cast<std::int32_t>(int_field(data, "SpawnX", spawn_x));
        spawn_y = static_cast<std::int32_t>(int_field(data, "SpawnY", spawn_y));
        spawn_z = static_cast<std::int32_t>(int_field(data, "SpawnZ", spawn_z));
        world_time = int_field(data, "Time", world_time);
        day_time = int_field(data, "DayTime", world_time);
        last_played = int_field(data, "LastPlayed", last_played);
        size_on_disk = int_field(data, "SizeOnDisk", size_on_disk);
        level_name = string_field(data, "LevelName", level_name);
        save_version = static_cast<std::int32_t>(int_field(data, "version", save_version));
        clear_weather_time = static_cast<std::int32_t>(int_field(data, "clearWeatherTime", clear_weather_time));
        rain_time = static_cast<std::int32_t>(int_field(data, "rainTime", rain_time));
        raining = bool_field(data, "raining", raining);
        thunder_time = static_cast<std::int32_t>(int_field(data, "thunderTime", thunder_time));
        thundering = bool_field(data, "thundering", thundering);
        hardcore = bool_field(data, "hardcore", hardcore);
        initialized = data.find("initialized") == nullptr ? true : bool_field(data, "initialized", true);
        allow_commands = data.find("allowCommands") == nullptr
            ? game_type == 1
            : bool_field(data, "allowCommands", false);
        if (data.find("Difficulty") != nullptr) {
            difficulty = get_difficulty_by_id(static_cast<int>(int_field(data, "Difficulty", 2)));
        }
        difficulty_locked = bool_field(data, "DifficultyLocked", difficulty_locked);

        if (const auto* rules = data.find("GameRules"); rules != nullptr) {
            if (const auto* fields = rules->get_if<nbt::Compound>(); fields != nullptr) {
                game_rules = GameRules{};
                game_rules.clear();
                for (const auto& [key, value] : *fields) {
                    game_rules.set_or_create(key, value.text().value_or(""));
                }
            }
        }

        border_center_x = number_field(data, "BorderCenterX", border_center_x);
        border_center_z = number_field(data, "BorderCenterZ", border_center_z);
        border_diameter = number_field(data, "BorderSize", border_diameter);
        border_size_lerp_time = int_field(data, "BorderSizeLerpTime", border_size_lerp_time);
        border_size_lerp_target = number_field(data, "BorderSizeLerpTarget", border_size_lerp_target);
        border_safe_zone = number_field(data, "BorderSafeZone", border_safe_zone);
        border_damage_amount = number_field(data, "BorderDamagePerBlock", border_damage_amount);
        border_warning_distance = static_cast<std::int32_t>(number_field(
            data, "BorderWarningBlocks", static_cast<double>(border_warning_distance)));
        border_warning_time = static_cast<std::int32_t>(number_field(
            data, "BorderWarningTime", static_cast<double>(border_warning_time)));
        if (const auto* dimension_data = data.find("DimensionData");
            dimension_data != nullptr && dimension_data->is(nbt::Tag::compound)) {
            dimension_data_ = *dimension_data;
        }
        if (const auto* player = data.find("Player"); player != nullptr && player->is(nbt::Tag::compound)) {
            player_tag_ = *player;
            dimension_ = static_cast<std::int32_t>(int_field(*player, "Dimension", dimension_));
        }
    }

    [[nodiscard]] nbt::Value write_to_nbt() const {
        nbt::Value data = source_data_.is(nbt::Tag::compound)
            ? source_data_
            : nbt::make_compound({});

        nbt::Compound version;
        version.emplace_back("Name", nbt::make_string("1.12.2"));
        version.emplace_back("Id", nbt::make_i32(1343));
        version.emplace_back("Snapshot", nbt::make_i8(0));
        data.set("Version", nbt::make_compound(std::move(version)));
        data.set("DataVersion", nbt::make_i32(1343));
        data.set("RandomSeed", nbt::make_i64(random_seed));
        data.set("generatorName", nbt::make_string(world_type_name(terrain_type).data()));
        data.set("generatorVersion", nbt::make_i32(world_type_version(terrain_type)));
        data.set("generatorOptions", nbt::make_string(generator_options));
        data.set("GameType", nbt::make_i32(game_type));
        data.set("MapFeatures", nbt::make_i8(map_features_enabled ? 1 : 0));
        data.set("SpawnX", nbt::make_i32(spawn_x));
        data.set("SpawnY", nbt::make_i32(spawn_y));
        data.set("SpawnZ", nbt::make_i32(spawn_z));
        data.set("Time", nbt::make_i64(world_time));
        data.set("DayTime", nbt::make_i64(day_time));
        data.set("SizeOnDisk", nbt::make_i64(size_on_disk));
        data.set("LastPlayed", nbt::make_i64(now_ms()));
        data.set("LevelName", nbt::make_string(level_name));
        data.set("version", nbt::make_i32(save_version));
        data.set("clearWeatherTime", nbt::make_i32(clear_weather_time));
        data.set("rainTime", nbt::make_i32(rain_time));
        data.set("raining", nbt::make_i8(raining ? 1 : 0));
        data.set("thunderTime", nbt::make_i32(thunder_time));
        data.set("thundering", nbt::make_i8(thundering ? 1 : 0));
        data.set("hardcore", nbt::make_i8(hardcore ? 1 : 0));
        data.set("allowCommands", nbt::make_i8(allow_commands ? 1 : 0));
        data.set("initialized", nbt::make_i8(initialized ? 1 : 0));
        data.set("BorderCenterX", nbt::make_f64(border_center_x));
        data.set("BorderCenterZ", nbt::make_f64(border_center_z));
        data.set("BorderSize", nbt::make_f64(border_diameter));
        data.set("BorderSizeLerpTime", nbt::make_i64(border_size_lerp_time));
        data.set("BorderSafeZone", nbt::make_f64(border_safe_zone));
        data.set("BorderDamagePerBlock", nbt::make_f64(border_damage_amount));
        data.set("BorderSizeLerpTarget", nbt::make_f64(border_size_lerp_target));
        // 原版 1.12.2 意外以 TAG_Double 写入这两个 int 字段。
        data.set("BorderWarningBlocks", nbt::make_f64(static_cast<double>(border_warning_distance)));
        data.set("BorderWarningTime", nbt::make_f64(static_cast<double>(border_warning_time)));
        data.set("Difficulty", nbt::make_i8(static_cast<std::int8_t>(static_cast<int>(difficulty))));
        data.set("DifficultyLocked", nbt::make_i8(difficulty_locked ? 1 : 0));

        nbt::Compound rules;
        for (const auto& [key, value] : game_rules.get_rules()) {
            rules.emplace_back(key, nbt::make_string(value.value));
        }
        data.set("GameRules", nbt::make_compound(std::move(rules)));
        // 维度数据：原版 updateTagCompound 只写非空条目
        const auto* dimensions = dimension_data_.get_if<nbt::Compound>();
        if (dimensions != nullptr && !dimensions->empty()) {
            data.set("DimensionData", dimension_data_);
        } else if (data.find("DimensionData") == nullptr) {
            data.set("DimensionData", nbt::make_compound({}));
        }

        nbt::Compound root;
        root.emplace_back("Data", std::move(data));
        return nbt::make_compound(std::move(root));
    }

    void read_root(const nbt::Value& root) {
        if (const auto* data = root.find("Data"); data != nullptr) read_from_nbt(*data);
    }

    [[nodiscard]] const nbt::Value& dimension_data() const noexcept { return dimension_data_; }
    [[nodiscard]] const nbt::Value& player_tag() const noexcept { return player_tag_; }

    void set_dimension_data(int dimension, nbt::Value data) {
        if (!dimension_data_.is(nbt::Tag::compound)) dimension_data_ = nbt::make_compound({});
        dimension_data_.set(std::to_string(dimension), std::move(data));
    }

private:
    nbt::Value dimension_data_ = nbt::make_compound({});
    nbt::Value player_tag_ = nbt::make_compound({});
    std::int32_t dimension_{0};

    [[nodiscard]] static std::int64_t now_ms() noexcept {
        using namespace std::chrono;
        return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
    }

    [[nodiscard]] static bool bool_field(const nbt::Value& data, std::string_view key, bool fallback) {
        const auto* value = data.find(key);
        return value == nullptr ? fallback : value->scalar().value_or(fallback ? 1 : 0) != 0;
    }

    [[nodiscard]] static std::int64_t int_field(const nbt::Value& data, std::string_view key,
                                                std::int64_t fallback) {
        const auto* value = data.find(key);
        return value == nullptr ? fallback : value->scalar().value_or(fallback);
    }

    [[nodiscard]] static double number_field(const nbt::Value& data, std::string_view key,
                                             double fallback) {
        const auto* value = data.find(key);
        if (value == nullptr) return fallback;
        if (const auto* v = value->get_if<double>(); v != nullptr) return *v;
        if (const auto* v = value->get_if<float>(); v != nullptr) return static_cast<double>(*v);
        if (const auto integer = value->scalar(); integer.has_value()) return static_cast<double>(*integer);
        return fallback;
    }

    [[nodiscard]] static std::string string_field(const nbt::Value& data, std::string_view key,
                                                  std::string fallback) {
        const auto* value = data.find(key);
        if (value == nullptr) return fallback;
        const auto text = value->text();
        return text.has_value() ? std::string{*text} : fallback;
    }

    nbt::Value source_data_ = nbt::make_compound({});
};

// 1.12.2 原版存档列表摘要（WorldSummary）。
class WorldSummary {
public:
    std::int64_t random_seed{0};
    std::string level_name;
    std::int64_t size_on_disk{0};
    std::int64_t last_played{0};
    std::string version_name{"1.12.2"};
    std::int32_t version_id{1343};
    bool requires_conversion{false};

    static WorldSummary from_root(const nbt::Value& root) {
        WorldSummary summary;
        const auto* data = root.find("Data");
        if (data == nullptr) return summary;
        if (const auto* value = data->find("RandomSeed"); value != nullptr) summary.random_seed = value->scalar().value_or(0);
        if (const auto* value = data->find("LevelName"); value != nullptr) summary.level_name = std::string{value->text().value_or("")};
        if (const auto* value = data->find("SizeOnDisk"); value != nullptr) summary.size_on_disk = value->scalar().value_or(0);
        if (const auto* value = data->find("LastPlayed"); value != nullptr) summary.last_played = value->scalar().value_or(0);
        if (const auto* version = data->find("Version"); version != nullptr) {
            if (const auto* name = version->find("Name"); name != nullptr) summary.version_name = std::string{name->text().value_or(summary.version_name)};
            if (const auto* id = version->find("Id"); id != nullptr) summary.version_id = static_cast<std::int32_t>(id->scalar().value_or(summary.version_id));
        }
        summary.requires_conversion = data->find("Version") == nullptr;
        return summary;
    }
};

}  // namespace cyane::world::storage
