#include "cyane/item/crafting.hpp"

#include <algorithm>
#include <format>

namespace cyane::item {
namespace {

[[nodiscard]] bool same_item(const ItemStack& a, std::int16_t id) noexcept {
    if (id == 0) {
        return a.empty();
    }
    return !a.empty() && a.id == id;
}

// shaped 匹配：图案在 rows*cols 网格内平移对齐（含 1:1），图案外格子必须为空
[[nodiscard]] bool match_shaped(const Recipe& r, const std::array<ItemStack, 9>& grid,
                                std::size_t rows, std::size_t cols) {
    if (r.rows > rows || r.cols > cols) {
        return false;
    }
    for (std::size_t off_row = 0; off_row + r.rows <= rows; ++off_row) {
        for (std::size_t off_col = 0; off_col + r.cols <= cols; ++off_col) {
            bool ok = true;
            for (std::size_t row = 0; row < rows && ok; ++row) {
                for (std::size_t col = 0; col < cols; ++col) {
                    const bool inside = row >= off_row && row < off_row + r.rows &&
                                        col >= off_col && col < off_col + r.cols;
                    const std::size_t cell = row * 3 + col;
                    if (inside) {
                        const auto ingredient =
                            r.grid[(row - off_row) * r.cols + (col - off_col)];
                        if (!same_item(grid[cell], ingredient)) {
                            ok = false;
                            break;
                        }
                    } else if (!grid[cell].empty()) {
                        ok = false;
                        break;
                    }
                }
            }
            if (ok) {
                return true;
            }
        }
    }
    return false;
}

// shapeless 匹配：非空格子 id 多重集与成分一致
[[nodiscard]] bool match_shapeless(const Recipe& r, const std::array<ItemStack, 9>& grid) {
    std::array<std::int16_t, 9> present{};
    std::size_t count = 0;
    for (const auto& cell : grid) {
        if (!cell.empty()) {
            if (count >= present.size()) {
                return false;
            }
            present[count++] = cell.id;
        }
    }
    if (count != r.ingredient_count) {
        return false;
    }
    std::array<bool, 9> consumed{};
    for (std::size_t i = 0; i < count; ++i) {
        bool found = false;
        for (std::size_t j = 0; j < r.ingredient_count; ++j) {
            if (!consumed[j] && r.grid[j] == present[i]) {
                consumed[j] = true;
                found = true;
                break;
            }
        }
        if (!found) {
            return false;
        }
    }
    return true;
}

}  // namespace

Result<void> CraftingRegistry::load_config(const Config& config) {
    const auto* tables = config.table_array("recipe");
    if (tables == nullptr) {
        return make_error(ErrorCode::config, "no [[recipe]] tables found");
    }
    recipes_.clear();
    recipes_.reserve(tables->size());
    for (const auto& table : *tables) {
        Recipe r;
        const auto type_it = table.find("type");
        const bool shaped = type_it != table.end() &&
                            std::holds_alternative<std::string>(type_it->second) &&
                            std::get<std::string>(type_it->second) == "shaped";

        const auto get_int = [&table](std::string_view key) -> std::optional<std::int64_t> {
            const auto it = table.find(key);
            if (it == table.end() || !std::holds_alternative<std::int64_t>(it->second)) {
                return std::nullopt;
            }
            return std::get<std::int64_t>(it->second);
        };

        const auto result_id = get_int("result");
        if (!result_id || *result_id <= 0 || *result_id > 0x7FFF) {
            return make_error(ErrorCode::config, "recipe: missing/invalid 'result'");
        }
        r.result_id = static_cast<std::int16_t>(*result_id);
        r.result_count = static_cast<std::uint8_t>(
            std::clamp<std::int64_t>(get_int("count").value_or(1), 1, 64));

        const auto pattern_it = table.find("pattern");
        if (pattern_it == table.end() || !std::holds_alternative<Config::Array>(pattern_it->second)) {
            return make_error(ErrorCode::config, "recipe: missing 'pattern' array");
        }
        const auto& pattern = std::get<Config::Array>(pattern_it->second);
        if (pattern.size() > 9) {
            return make_error(ErrorCode::config, "recipe: pattern larger than 3x3");
        }
        for (std::size_t i = 0; i < pattern.size(); ++i) {
            std::int64_t id = 0;
            const auto [ptr, ec] = std::from_chars(pattern[i].data(),
                                                   pattern[i].data() + pattern[i].size(), id);
            if (ec != std::errc{} || ptr != pattern[i].data() + pattern[i].size() || id < 0 || id > 0x7FFF) {
                return make_error(ErrorCode::config,
                                  std::format("recipe: invalid pattern entry '{}'", pattern[i]));
            }
            r.grid[i] = static_cast<std::int16_t>(id);
        }

        if (shaped) {
            const auto rows = get_int("rows");
            const auto cols = get_int("cols");
            if (!rows || !cols || *rows < 1 || *rows > 3 || *cols < 1 || *cols > 3) {
                return make_error(ErrorCode::config, "shaped recipe: missing/invalid 'rows'/'cols'");
            }
            r.rows = static_cast<std::size_t>(*rows);
            r.cols = static_cast<std::size_t>(*cols);
            if (pattern.size() != r.rows * r.cols) {
                return make_error(ErrorCode::config,
                                   std::format("shaped recipe: pattern size {} != rows*cols {}",
                                               pattern.size(), r.rows * r.cols));
            }
        } else {
            r.ingredient_count = pattern.size();
        }
        recipes_.push_back(r);
    }
    return {};
}

std::optional<Recipe> CraftingRegistry::find(const std::array<ItemStack, 9>& grid,
                                             std::size_t rows, std::size_t cols) const {
    const bool empty_grid = std::all_of(grid.begin(), grid.end(),
                                        [](const ItemStack& s) { return s.empty(); });
    if (empty_grid) {
        return std::nullopt;
    }
    for (const auto& r : recipes_) {
        if (r.shaped() ? match_shaped(r, grid, rows, cols) : match_shapeless(r, grid)) {
            return r;
        }
    }
    return std::nullopt;
}

std::optional<Recipe> CraftingRegistry::find_2x2(const std::array<ItemStack, 4>& cells) const {
    std::array<ItemStack, 9> grid{};
    for (std::size_t i = 0; i < 4; ++i) {
        grid[(i / 2) * 3 + (i % 2)] = cells[i];
    }
    return find(grid, 2, 2);
}

Result<void> CraftingRegistry::load_furnace_config(
    const Config& config,
    std::unordered_map<std::int16_t, std::int32_t>& fuel,
    std::unordered_map<std::int16_t, std::pair<std::int16_t, std::uint8_t>>& smelting) const {
    fuel.clear();
    smelting.clear();
    if (const auto* tables = config.table_array("smelting")) {
        for (const auto& table : *tables) {
            const auto in_it = table.find("input");
            const auto out_it = table.find("result");
            if (in_it == table.end() || out_it == table.end() ||
                !std::holds_alternative<std::int64_t>(in_it->second) ||
                !std::holds_alternative<std::int64_t>(out_it->second)) {
                return make_error(ErrorCode::config, "smelting: need integer 'input' and 'result'");
            }
            const auto in_id = std::get<std::int64_t>(in_it->second);
            const auto out_id = std::get<std::int64_t>(out_it->second);
            if (in_id <= 0 || in_id > 0x7FFF || out_id <= 0 || out_id > 0x7FFF) {
                return make_error(ErrorCode::config, "smelting: item id out of range");
            }
            smelting[static_cast<std::int16_t>(in_id)] =
                {static_cast<std::int16_t>(out_id), 1};
        }
    }
    if (const auto* tables = config.table_array("fuel")) {
        for (const auto& table : *tables) {
            const auto item_it = table.find("item");
            const auto ticks_it = table.find("ticks");
            if (item_it == table.end() || ticks_it == table.end() ||
                !std::holds_alternative<std::int64_t>(item_it->second) ||
                !std::holds_alternative<std::int64_t>(ticks_it->second)) {
                return make_error(ErrorCode::config, "fuel: need integer 'item' and 'ticks'");
            }
            const auto item_id = std::get<std::int64_t>(item_it->second);
            const auto ticks = std::get<std::int64_t>(ticks_it->second);
            if (item_id <= 0 || item_id > 0x7FFF || ticks <= 0) {
                return make_error(ErrorCode::config, "fuel: invalid item id or ticks");
            }
            fuel[static_cast<std::int16_t>(item_id)] = static_cast<std::int32_t>(ticks);
        }
    }
    return {};
}

}  // namespace cyane::item
