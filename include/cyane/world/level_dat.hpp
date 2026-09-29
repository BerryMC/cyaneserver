#pragma once

#include <cstdint>
#include <filesystem>

#include "cyane/core/error.hpp"

namespace cyane::world {

// level.dat 里我们关心的最小子集（root → Data → 字段）
struct LevelInfo {
    std::int32_t spawn_x{0};
    std::int32_t spawn_y{4};
    std::int32_t spawn_z{0};
};

// 读取 <world>/level.dat（gzip NBT）；文件不存在返回默认值
[[nodiscard]] Result<LevelInfo> load_level_dat(const std::filesystem::path& world_dir);

// 首启建档：<world>/level.dat 缺失时写最小集（Time/Version/GameType/generator，
// gzip NBT，与原版格式一致）；已存在则不动。返回是否写入了新文件。
[[nodiscard]] Result<bool> ensure_level_dat(const std::filesystem::path& world_dir,
                                            std::int32_t game_type);

} // namespace cyane::world