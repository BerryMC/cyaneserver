#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace cyane::world {

// 1.12.2 原版世界类型（WorldType 逐行转写）：id / name / 生成器版本
enum class WorldType : std::uint8_t {
    Default = 0,
    Flat = 1,
    LargeBiomes = 2,
    Amplified = 3,
    Customized = 4,
    DebugAllBlockStates = 5,
    Default1_1 = 8,
};

[[nodiscard]] constexpr std::string_view world_type_name(WorldType type) noexcept {
    switch (type) {
        case WorldType::Default: return "default";
        case WorldType::Flat: return "flat";
        case WorldType::LargeBiomes: return "largeBiomes";
        case WorldType::Amplified: return "amplified";
        case WorldType::Customized: return "customized";
        case WorldType::DebugAllBlockStates: return "debug_all_block_states";
        case WorldType::Default1_1: return "default_1_1";
    }
    return "default";
}

// 生成器版本：level.dat 的 generatorVersion，1.1 旧图为 0，其余为 1
[[nodiscard]] constexpr int world_type_version(WorldType type) noexcept {
    return type == WorldType::Default1_1 ? 0 : 1;
}

// 是否允许在单人创建界面选择（1.1 旧图不允许新建）
[[nodiscard]] constexpr bool world_type_can_be_created(WorldType type) noexcept {
    return type != WorldType::Default1_1;
}

[[nodiscard]] constexpr bool world_type_is_versioned(WorldType type) noexcept {
    return type == WorldType::Default;
}

[[nodiscard]] constexpr bool world_type_has_info_notice(WorldType type) noexcept {
    return type == WorldType::Amplified;
}

// 对齐原版按名字查表（大小写不敏感由调用方处理，这里按原版精确名匹配）
[[nodiscard]] constexpr std::optional<WorldType> world_type_by_name(std::string_view name) noexcept {
    if (name == "default") return WorldType::Default;
    if (name == "flat") return WorldType::Flat;
    if (name == "largeBiomes") return WorldType::LargeBiomes;
    if (name == "amplified") return WorldType::Amplified;
    if (name == "customized") return WorldType::Customized;
    if (name == "debug_all_block_states") return WorldType::DebugAllBlockStates;
    if (name == "default_1_1") return WorldType::Default1_1;
    return std::nullopt;
}

// 1.12.2 原版世界设置（WorldSettings）
// 对应 level.dat 里的 seed / GameType / MapFeatures / hardcore / generatorName / generatorOptions。
class WorldSettings {
public:
    constexpr WorldSettings(std::int64_t seed, std::uint8_t game_type, bool map_features_enabled,
                            bool hardcore, WorldType terrain_type) noexcept
        : seed_(seed),
          game_type_(game_type),
          map_features_enabled_(map_features_enabled),
          hardcore_enabled_(hardcore),
          terrain_type_(terrain_type) {}

    [[nodiscard]] constexpr std::int64_t seed() const noexcept { return seed_; }
    [[nodiscard]] constexpr std::uint8_t game_type() const noexcept { return game_type_; }
    [[nodiscard]] constexpr bool map_features_enabled() const noexcept { return map_features_enabled_; }
    [[nodiscard]] constexpr bool hardcore_enabled() const noexcept { return hardcore_enabled_; }
    [[nodiscard]] constexpr WorldType terrain_type() const noexcept { return terrain_type_; }
    [[nodiscard]] constexpr bool bonus_chest_enabled() const noexcept { return bonus_chest_enabled_; }
    [[nodiscard]] constexpr bool commands_allowed() const noexcept { return commands_allowed_; }
    [[nodiscard]] constexpr const std::string& generator_options() const noexcept { return generator_options_; }

    constexpr WorldSettings& enable_bonus_chest() noexcept {
        bonus_chest_enabled_ = true;
        return *this;
    }

    constexpr WorldSettings& enable_commands() noexcept {
        commands_allowed_ = true;
        return *this;
    }

    WorldSettings& set_generator_options(std::string options) {
        generator_options_ = std::move(options);
        return *this;
    }

    // vanilla isHardcoreModeEnabled 受 gameType 影响：困难核心只对生存生效
    [[nodiscard]] constexpr bool is_hardcore_mode_enabled() const noexcept {
        return hardcore_enabled_ && game_type_ == 0;
    }

private:
    std::int64_t seed_;
    std::uint8_t game_type_;
    bool map_features_enabled_;
    bool hardcore_enabled_;
    WorldType terrain_type_;
    bool bonus_chest_enabled_{false};
    bool commands_allowed_{false};
    std::string generator_options_;
};

}  // namespace cyane::world
