#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <string_view>

namespace cyane::world {

// 1.12.2 原版世界难度枚举（EnumDifficulty 逐行转写）
enum class EnumDifficulty : std::uint8_t {
    Peaceful = 0,
    Easy = 1,
    Normal = 2,
    Hard = 3
};

[[nodiscard]] constexpr std::string_view get_difficulty_name(EnumDifficulty diff) noexcept {
    switch (diff) {
        case EnumDifficulty::Peaceful: return "peaceful";
        case EnumDifficulty::Easy: return "easy";
        case EnumDifficulty::Normal: return "normal";
        case EnumDifficulty::Hard: return "hard";
    }
    return "normal";
}

[[nodiscard]] constexpr EnumDifficulty get_difficulty_by_id(int id) noexcept {
    switch (id) {
        case 0: return EnumDifficulty::Peaceful;
        case 1: return EnumDifficulty::Easy;
        case 2: return EnumDifficulty::Normal;
        case 3: return EnumDifficulty::Hard;
        default: return EnumDifficulty::Normal;
    }
}

// 1.12.2 原版局部区域难度计算器（DifficultyInstance 逐行转写）
class DifficultyInstance {
public:
    DifficultyInstance(EnumDifficulty world_difficulty, std::int64_t world_time, std::int64_t chunk_inhabited_time, float moon_phase_factor)
        : world_difficulty_(world_difficulty),
          additional_difficulty_(calculate_additional_difficulty(world_difficulty, world_time, chunk_inhabited_time, moon_phase_factor)) {}

    [[nodiscard]] constexpr float get_additional_difficulty() const noexcept {
        return additional_difficulty_;
    }

    [[nodiscard]] constexpr float get_clamped_additional_difficulty() const noexcept {
        if (additional_difficulty_ < 2.0f) return 0.0f;
        if (additional_difficulty_ > 4.0f) return 1.0f;
        return (additional_difficulty_ - 2.0f) / 2.0f;
    }

    [[nodiscard]] constexpr EnumDifficulty get_difficulty() const noexcept {
        return world_difficulty_;
    }

private:
    [[nodiscard]] static float calculate_additional_difficulty(EnumDifficulty difficulty, std::int64_t world_time,
                                                              std::int64_t chunk_inhabited_time, float moon_phase_factor) noexcept {
        if (difficulty == EnumDifficulty::Peaceful) return 0.0f;
        const bool is_hard = difficulty == EnumDifficulty::Hard;
        float factor = 0.75f;

        // 世界时间因子（最高 0.25）
        const float time_factor = std::clamp(static_cast<float>(world_time - 72000L) / 1440000.0f, 0.0f, 1.0f) * 0.25f;
        factor += time_factor;

        // 区块居住时间与月相因子（最高 1.0）
        float chunk_factor = std::clamp(static_cast<float>(chunk_inhabited_time) / 3600000.0f, 0.0f, 1.0f) * (is_hard ? 1.0f : 0.75f);
        chunk_factor += std::clamp(moon_phase_factor * 0.25f, 0.0f, time_factor);

        if (difficulty == EnumDifficulty::Easy) {
            chunk_factor *= 0.5f;
        }

        factor += chunk_factor;
        float base_val = static_cast<float>(static_cast<int>(difficulty));
        return base_val * factor;
    }

    EnumDifficulty world_difficulty_;
    float additional_difficulty_;
};

}  // namespace cyane::world
