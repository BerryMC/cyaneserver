#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include "cyane/world/difficulty.hpp"
#include "cyane/world/game_rules.hpp"
#include "cyane/world/nbt.hpp"
#include "cyane/world/world_border.hpp"
#include "cyane/world/world_settings.hpp"
#include "cyane/world/world_types.hpp"

namespace cyane::world::storage {

// 1.12.2 原版世界元数据（WorldInfo 逐字段转写）
// 对应 level.dat 的根 compound 下 "Data" 的全部常用字段；读写严格按原版键名与类型。
class WorldInfo {
public:
    // ---- 版本 ----
    std::int32_t version_id{1343};             // Version.Id（1.12.2 = 1343）
    std::string version_name{"1.12.2"};        // Version.Name
    bool initialized{false};                   // initialized

    // ---- 名称与生成器 ----
    std::string level_name{"world"};           // LevelName
    std::string generator_name{"default"};     // generatorName
    std::int32_t generator_version{1};         // generatorVersion
    std::string generator_options;             // generatorOptions
    WorldType terrain_type{WorldType::Default};
    bool map_features_enabled{true};           // MapFeatures
    bool allow_commands{true};                 // allowCommands
    bool hardcore{false};                      // hardcore

    // ---- 游戏状态 ----
    std::uint8_t game_type{0};                 // GameType
    EnumDifficulty difficulty{EnumDifficulty::Normal};
    GameRules game_rules{};
    std::int64_t random_seed{0};               // RandomSeed
    std::int64_t world_time{0};                // Time（累计游戏时间）
    std::int64_t day_time{0};                  // DayTime（0-24000 循环）
    std::int64_t last_played{0};               // LastPlayed（毫秒）

    // ---- 出生点 ----
    std::int32_t spawn_x{0};
    std::int32_t spawn_y{64};
    std::int32_t spawn_z{0};

    // ---- 天气 ----
    bool raining{false};
    std::int32_t rain_time{0};
    bool thundering{false};
    std::int32_t thunder_time{0};
    std::int32_t clear_weather_time{0};

    // ---- 世界边界 ----
    double border_center_x{0.0};
    double border_center_z{0.0};
    double border_diameter{5.9999968E7};
    double border_damage_amount{0.2};
    double border_damage_buffer{5.0};
    std::int64_t border_warning_time{15};
    std::int64_t border_warning_distance{5};

    [[nodiscard]] std::uint8_t game_mode() const noexcept { return game_type; }
    void set_game_mode(std::uint8_t mode) noexcept { game_type = mode; }

    // ---- NBT ----
    void read_from_nbt(const nbt::Value& data) {
        if (const auto* v = data.find("Version"); v != nullptr) {
            if (const auto* id = v->find("Id"); id != nullptr) version_id = static_cast<std::int32_t>(id->scalar().value_or(version_id));
            if (const auto* name = v->find("Name"); name != nullptr) version_name = std::string{name->text().value_or(version_name)};
        }
        initialized = bool_field(data, "initialized", initialized);
        level_name = string_field(data, "LevelName", level_name);
        generator_name = string_field(data, "generatorName", generator_name);
        generator_version = static_cast<std::int32_t>(int_field(data, "generatorVersion", generator_version));
        generator_options = string_field(data, "generatorOptions", generator_options);
        if (const auto name = world_type_by_name(generator_name); name.has_value()) terrain_type = *name;
        map_features_enabled = bool_field(data, "MapFeatures", map_features_enabled);
        allow_commands = bool_field(data, "allowCommands", allow_commands);
        hardcore = bool_field(data, "hardcore", hardcore);
        game_type = static_cast<std::uint8_t>(int_field(data, "GameType", game_type));
        difficulty = get_difficulty_by_id(static_cast<int>(int_field(data, "Difficulty", static_cast<std::int64_t>(static_cast<int>(difficulty)))));
        random_seed = int_field(data, "RandomSeed", random_seed);
        world_time = int_field(data, "Time", world_time);
        day_time = int_field(data, "DayTime", day_time);
        last_played = int_field(data, "LastPlayed", last_played);
        spawn_x = static_cast<std::int32_t>(int_field(data, "SpawnX", spawn_x));
        spawn_y = static_cast<std::int32_t>(int_field(data, "SpawnY", spawn_y));
        spawn_z = static_cast<std::int32_t>(int_field(data, "SpawnZ", spawn_z));
        raining = bool_field(data, "raining", raining);
        rain_time = static_cast<std::int32_t>(int_field(data, "rainTime", rain_time));
        thundering = bool_field(data, "thundering", thundering);
        thunder_time = static_cast<std::int32_t>(int_field(data, "thunderTime", thunder_time));
        clear_weather_time = static_cast<std::int32_t>(int_field(data, "clearWeatherTime", clear_weather_time));

        if (const auto* rules = data.find("GameRules"); rules != nullptr) {
            if (const auto* map = rules->get_if<nbt::Compound>(); map != nullptr) {
                for (const auto& [key, value] : *map) {
                    game_rules.set_or_create(key, value.text().value_or(""));
                }
            }
        }

        if (const auto* border = data.find("Border"); border != nullptr) {
            if (const auto* v = border->find("CenterX"); v != nullptr) border_center_x = static_cast<double>(v->scalar().value_or(0));
            if (const auto* v = border->find("CenterZ"); v != nullptr) border_center_z = static_cast<double>(v->scalar().value_or(0));
            if (const auto* v = border->find("Size"); v != nullptr) border_diameter = static_cast<double>(v->scalar().value_or(0));
            if (const auto* v = border->find("DamageAmount"); v != nullptr) border_damage_amount = static_cast<double>(v->scalar().value_or(0));
            if (const auto* v = border->find("DamageBuffer"); v != nullptr) border_damage_buffer = static_cast<double>(v->scalar().value_or(0));
            if (const auto* v = border->find("WarningTime"); v != nullptr) border_warning_time = v->scalar().value_or(15);
            if (const auto* v = border->find("WarningDistance"); v != nullptr) border_warning_distance = v->scalar().value_or(5);
        }
    }

    [[nodiscard]] nbt::Value write_to_nbt() const {
        nbt::Compound data;
        nbt::Compound version;
        version.emplace_back("Id", nbt::make_i32(version_id));
        version.emplace_back("Name", nbt::make_string(version_name));
        data.emplace_back("Version", nbt::make_compound(std::move(version)));
        data.emplace_back("DataVersion", nbt::make_i32(1343));
        data.emplace_back("initialized", nbt::make_i8(initialized ? 1 : 0));
        data.emplace_back("LevelName", nbt::make_string(level_name));
        data.emplace_back("generatorName", nbt::make_string(generator_name));
        data.emplace_back("generatorVersion", nbt::make_i32(generator_version));
        data.emplace_back("generatorOptions", nbt::make_string(generator_options));
        data.emplace_back("MapFeatures", nbt::make_i8(map_features_enabled ? 1 : 0));
        data.emplace_back("allowCommands", nbt::make_i8(allow_commands ? 1 : 0));
        data.emplace_back("hardcore", nbt::make_i8(hardcore ? 1 : 0));
        data.emplace_back("GameType", nbt::make_i32(game_type));
        data.emplace_back("Difficulty", nbt::make_i8(static_cast<std::int8_t>(static_cast<int>(difficulty))));
        data.emplace_back("DifficultyLocked", nbt::make_i8(0));
        data.emplace_back("RandomSeed", nbt::make_i64(random_seed));
        data.emplace_back("Time", nbt::make_i64(world_time));
        data.emplace_back("DayTime", nbt::make_i64(day_time));
        data.emplace_back("LastPlayed", nbt::make_i64(last_played));
        data.emplace_back("SpawnX", nbt::make_i32(spawn_x));
        data.emplace_back("SpawnY", nbt::make_i32(spawn_y));
        data.emplace_back("SpawnZ", nbt::make_i32(spawn_z));
        data.emplace_back("raining", nbt::make_i8(raining ? 1 : 0));
        data.emplace_back("rainTime", nbt::make_i32(rain_time));
        data.emplace_back("thundering", nbt::make_i8(thundering ? 1 : 0));
        data.emplace_back("thunderTime", nbt::make_i32(thunder_time));
        data.emplace_back("clearWeatherTime", nbt::make_i32(clear_weather_time));

        nbt::Compound rules;
        for (const auto& [key, value] : game_rules.get_rules()) {
            rules.emplace_back(key, nbt::make_string(value.value));
        }
        data.emplace_back("GameRules", nbt::make_compound(std::move(rules)));

        nbt::Compound border;
        border.emplace_back("CenterX", nbt::make_f64(border_center_x));
        border.emplace_back("CenterZ", nbt::make_f64(border_center_z));
        border.emplace_back("Size", nbt::make_f64(border_diameter));
        border.emplace_back("DamageAmount", nbt::make_f64(border_damage_amount));
        border.emplace_back("DamageBuffer", nbt::make_f64(border_damage_buffer));
        border.emplace_back("WarningTime", nbt::make_i64(border_warning_time));
        border.emplace_back("WarningDistance", nbt::make_i64(border_warning_distance));
        data.emplace_back("Border", nbt::make_compound(std::move(border)));

        nbt::Compound root;
        root.emplace_back("Data", nbt::make_compound(std::move(data)));
        return nbt::make_compound(std::move(root));
    }

    // 从完整 level.dat 根 compound（含 "Data"）载入
    void read_root(const nbt::Value& root) {
        if (const auto* data = root.find("Data"); data != nullptr) read_from_nbt(*data);
    }

private:
    [[nodiscard]] static bool bool_field(const nbt::Value& data, std::string_view key, bool fallback) {
        const auto* v = data.find(key);
        return v == nullptr ? fallback : v->scalar().value_or(fallback ? 1 : 0) != 0;
    }

    [[nodiscard]] static std::int64_t int_field(const nbt::Value& data, std::string_view key, std::int64_t fallback) {
        const auto* v = data.find(key);
        return v == nullptr ? fallback : v->scalar().value_or(fallback);
    }

    [[nodiscard]] static std::string string_field(const nbt::Value& data, std::string_view key, std::string fallback) {
        const auto* v = data.find(key);
        if (v == nullptr) return fallback;
        const auto text = v->text();
        return text.has_value() ? std::string{*text} : fallback;
    }
};

// 1.12.2 原版存档列表摘要（WorldSummary 逐字段转写）
// 单人存档列表用它快速展示，不加载整份 level.dat。
class WorldSummary {
public:
    std::int64_t random_seed{0};
    std::string level_name;
    std::int64_t size_on_disk{0};
    std::int64_t last_played{0};
    std::string version_name{"1.12.2"};
    std::int32_t version_id{1343};
    bool requires_conversion{false};

    // 与 WorldInfo 同源字段，从 level.dat 根 compound 解析
    static WorldSummary from_root(const nbt::Value& root) {
        WorldSummary summary;
        const auto* data = root.find("Data");
        if (data == nullptr) return summary;
        if (const auto* v = data->find("RandomSeed"); v != nullptr) summary.random_seed = v->scalar().value_or(0);
        if (const auto* v = data->find("LevelName"); v != nullptr) summary.level_name = std::string{v->text().value_or("")};
        if (const auto* v = data->find("SizeOnDisk"); v != nullptr) summary.size_on_disk = v->scalar().value_or(0);
        if (const auto* v = data->find("LastPlayed"); v != nullptr) summary.last_played = v->scalar().value_or(0);
        if (const auto* version = data->find("Version"); version != nullptr) {
            if (const auto* name = version->find("Name"); name != nullptr) {
                summary.version_name = std::string{name->text().value_or(summary.version_name)};
            }
            if (const auto* id = version->find("Id"); id != nullptr) {
                summary.version_id = static_cast<std::int32_t>(id->scalar().value_or(summary.version_id));
            }
        }
        // 缺 Version 视为需要转换（原版 requiresConversion）
        summary.requires_conversion = data->find("Version") == nullptr;
        return summary;
    }
};

}  // namespace cyane::world::storage
