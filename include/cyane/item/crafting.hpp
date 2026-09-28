#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

#include "cyane/core/config.hpp"
#include "cyane/item/item_stack.hpp"

namespace cyane::item {

// 合成配方（1.12.2 子集）。
// shaped：rows*cols 图案，在 3x3 网格内任意平移对齐，图案外格子必须为空；0 = 该格必须为空。
// shapeless：成分 id 多重集匹配，忽略位置。
struct Recipe {
    std::size_t rows{0};  // shaped 为 1..3；shapeless 为 0
    std::size_t cols{0};
    std::array<std::int16_t, 9> grid{};  // shaped: 图案；shapeless: 成分列表
    std::size_t ingredient_count{0};
    std::int16_t result_id{-1};
    std::uint8_t result_count{1};

    [[nodiscard]] bool shaped() const noexcept { return rows > 0; }
};

// 配方注册表：从 Config 的 [[recipe]]/[[smelting]]/[[fuel]] 表格数组加载。
class CraftingRegistry {
public:
    CraftingRegistry() = default;

    // 从 config 文本加载全部 [[recipe]] 表；格式错误返回 error
    [[nodiscard]] cyane::Result<void> load_config(const cyane::Config& config);

    // 加载 [[smelting]]（input→result）与 [[fuel]]（item→ticks）表
    [[nodiscard]] cyane::Result<void> load_furnace_config(
        const cyane::Config& config,
        std::unordered_map<std::int16_t, std::int32_t>& fuel,
        std::unordered_map<std::int16_t, std::pair<std::int16_t, std::uint8_t>>& smelting) const;

    [[nodiscard]] bool empty() const noexcept { return recipes_.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return recipes_.size(); }

    // 3x3 网格匹配（rows/cols 为网格实际尺寸，2x2 合成格传 2,2）
    [[nodiscard]] std::optional<Recipe> find(const std::array<ItemStack, 9>& grid,
                                             std::size_t rows, std::size_t cols) const;

    // 2x2 玩家合成格（4 格映射到 3x3 左上角）
    [[nodiscard]] std::optional<Recipe> find_2x2(const std::array<ItemStack, 4>& cells) const;

private:
    std::vector<Recipe> recipes_;
};

}
