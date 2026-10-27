#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace cyane::world {

// 1.12.2 原版光照通道（EnumSkyBlock 逐行转写）
enum class EnumSkyBlock : std::uint8_t {
    Sky = 0,
    Block = 1
};

[[nodiscard]] constexpr int default_light_value(EnumSkyBlock channel) noexcept {
    return channel == EnumSkyBlock::Sky ? 15 : 0;
}

// 1.12.2 原版维度类型（DimensionType 逐行转写）
enum class DimensionType : std::int8_t {
    Overworld = 0,
    Nether = -1,
    TheEnd = 1
};

[[nodiscard]] constexpr std::int32_t dimension_id(DimensionType type) noexcept {
    return static_cast<std::int32_t>(type);
}

[[nodiscard]] constexpr std::string_view dimension_name(DimensionType type) noexcept {
    switch (type) {
        case DimensionType::Overworld: return "overworld";
        case DimensionType::Nether: return "the_nether";
        case DimensionType::TheEnd: return "the_end";
    }
    return "overworld";
}

// 目录后缀：下界/末地的 region 目录分别为 DIM-1 / DIM1，主世界为空
[[nodiscard]] constexpr std::string_view dimension_suffix(DimensionType type) noexcept {
    switch (type) {
        case DimensionType::Overworld: return "";
        case DimensionType::Nether: return "_nether";
        case DimensionType::TheEnd: return "_end";
    }
    return "";
}

// vanilla 只有主世界加载出生点区块
[[nodiscard]] constexpr bool dimension_loads_spawn(DimensionType type) noexcept {
    return type == DimensionType::Overworld;
}

[[nodiscard]] constexpr std::optional<DimensionType> dimension_by_id(std::int32_t id) noexcept {
    switch (id) {
        case 0: return DimensionType::Overworld;
        case -1: return DimensionType::Nether;
        case 1: return DimensionType::TheEnd;
        default: return std::nullopt;
    }
}

// 1.12.2 原版容器锁码（LockCode 逐行转写）：空串表示未上锁
class LockCode {
public:
    constexpr LockCode() noexcept = default;
    explicit LockCode(std::string code) : lock_(std::move(code)) {}

    [[nodiscard]] constexpr bool is_empty() const noexcept { return lock_.empty(); }
    [[nodiscard]] constexpr const std::string& get_lock() const noexcept { return lock_; }

    [[nodiscard]] static LockCode empty_code() noexcept { return LockCode{}; }

private:
    std::string lock_;
};

}  // namespace cyane::world
