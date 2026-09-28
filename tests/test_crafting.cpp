#include "cyane/item/crafting.hpp"
#include "cyane/item/item_stack.hpp"
#include "test_framework.hpp"

using cyane::item::CraftingRegistry;
using cyane::item::ItemStack;

namespace {
constexpr std::int16_t kLog = 17;
constexpr std::int16_t kPlanks = 5;
constexpr std::int16_t kStick = 280;
constexpr std::int16_t kCraftingTable = 58;
constexpr std::int16_t kFurnace = 61;
constexpr std::int16_t kTorch = 50;
constexpr std::int16_t kCoal = 263;
constexpr std::int16_t kCobblestone = 4;

// 数据驱动配方集（与 config/recipes.toml 内容一致的子集）
constexpr std::string_view kRecipesToml = R"toml(
[[recipe]]
type = "shapeless"
pattern = [17]
result = 5
count = 4

[[recipe]]
type = "shaped"
rows = 2
cols = 1
pattern = [5, 5]
result = 280
count = 4

[[recipe]]
type = "shaped"
rows = 2
cols = 2
pattern = [5, 5, 5, 5]
result = 58

[[recipe]]
type = "shaped"
rows = 3
cols = 3
pattern = [4, 4, 4, 4, 0, 4, 4, 4, 4]
result = 61

[[recipe]]
type = "shaped"
rows = 2
cols = 1
pattern = [263, 280]
result = 50
count = 4

[[recipe]]
type = "shaped"
rows = 3
cols = 3
pattern = [5, 5, 5, 0, 280, 0, 0, 280, 0]
result = 270
)toml";

[[nodiscard]] CraftingRegistry make_registry() {
    const auto config = cyane::Config::parse(kRecipesToml);
    CraftingRegistry registry;
    if (config) {
        (void)registry.load_config(*config);
    }
    return registry;
}
}  // namespace

CYANE_TEST(recipes_load_from_toml) {
    const auto registry = make_registry();
    CYANE_CHECK_EQ(registry.size(), static_cast<std::size_t>(6));
}

CYANE_TEST(recipe_log_to_planks_shapeless) {
    const auto registry = make_registry();
    // 单个原木 → 4 木板（shapeless），位置无关
    for (const std::size_t pos : {0U, 3U, 5U, 8U}) {
        std::array<ItemStack, 9> grid{};
        grid[pos] = ItemStack{kLog, 1, 0};
        const auto r = registry.find(grid, 3, 3);
        CYANE_CHECK(r.has_value());
        CYANE_CHECK_EQ(r->result_id, kPlanks);
        CYANE_CHECK_EQ(r->result_count, static_cast<std::uint8_t>(4));
    }
}

CYANE_TEST(recipe_sticks_shaped_with_translation) {
    const auto registry = make_registry();
    // 两块木板竖排 → 4 木棍；放在任何一列都能匹配（图案平移）
    for (const std::size_t col : {0U, 1U, 2U}) {
        std::array<ItemStack, 9> grid{};
        grid[col] = ItemStack{kPlanks, 1, 0};
        grid[3 + col] = ItemStack{kPlanks, 1, 0};
        const auto r = registry.find(grid, 3, 3);
        CYANE_CHECK(r.has_value());
        CYANE_CHECK_EQ(r->result_id, kStick);
        CYANE_CHECK_EQ(r->result_count, static_cast<std::uint8_t>(4));
    }
    // 横排不该匹配木棍
    std::array<ItemStack, 9> grid{};
    grid[0] = ItemStack{kPlanks, 1, 0};
    grid[1] = ItemStack{kPlanks, 1, 0};
    CYANE_CHECK(!registry.find(grid, 3, 3).has_value());
}

CYANE_TEST(recipe_crafting_table_2x2) {
    const auto registry = make_registry();
    std::array<ItemStack, 4> cells{};
    for (auto& c : cells) {
        c = ItemStack{kPlanks, 1, 0};
    }
    const auto r = registry.find_2x2(cells);
    CYANE_CHECK(r.has_value());
    CYANE_CHECK_EQ(r->result_id, kCraftingTable);
}

CYANE_TEST(recipe_furnace_requires_ring) {
    const auto registry = make_registry();
    // 8 圆石环 + 中心空 → 熔炉
    std::array<ItemStack, 9> grid{};
    for (std::size_t i = 0; i < 9; ++i) {
        if (i != 4) {
            grid[i] = ItemStack{kCobblestone, 1, 0};
        }
    }
    const auto r = registry.find(grid, 3, 3);
    CYANE_CHECK(r.has_value());
    CYANE_CHECK_EQ(r->result_id, kFurnace);
    // 中心被占用则不匹配
    grid[4] = ItemStack{kCobblestone, 1, 0};
    CYANE_CHECK(!registry.find(grid, 3, 3).has_value());
}

CYANE_TEST(recipe_torch_coal_over_stick) {
    const auto registry = make_registry();
    std::array<ItemStack, 9> grid{};
    grid[1] = ItemStack{kCoal, 1, 0};
    grid[4] = ItemStack{kStick, 1, 0};
    const auto r = registry.find(grid, 3, 3);
    CYANE_CHECK(r.has_value());
    CYANE_CHECK_EQ(r->result_id, kTorch);
    CYANE_CHECK_EQ(r->result_count, static_cast<std::uint8_t>(4));
}

CYANE_TEST(recipe_empty_grid_yields_nothing) {
    const auto registry = make_registry();
    std::array<ItemStack, 9> grid{};
    CYANE_CHECK(!registry.find(grid, 3, 3).has_value());
    std::array<ItemStack, 4> cells{};
    CYANE_CHECK(!registry.find_2x2(cells).has_value());
}

CYANE_TEST(recipe_extra_items_fail_shaped) {
    const auto registry = make_registry();
    // 图案外多出的物品导致不匹配
    std::array<ItemStack, 9> grid{};
    grid[1] = ItemStack{kCoal, 1, 0};
    grid[4] = ItemStack{kStick, 1, 0};
    grid[7] = ItemStack{kStick, 1, 0};  // 多余
    CYANE_CHECK(!registry.find(grid, 3, 3).has_value());
}

CYANE_TEST(recipe_invalid_toml_rejected) {
    // 缺 result 字段应被拒绝
    constexpr std::string_view kBad = R"toml(
[[recipe]]
type = "shapeless"
pattern = [17]
)toml";
    auto config = cyane::Config::parse(kBad);
    CYANE_CHECK(config.has_value());
    CraftingRegistry registry;
    const auto rc = registry.load_config(*config);
    CYANE_CHECK(!rc.has_value());
}

CYANE_TEST(recipe_pattern_size_mismatch_rejected) {
    constexpr std::string_view kBad = R"toml(
[[recipe]]
type = "shaped"
rows = 3
cols = 3
pattern = [4, 4, 4, 4, 0, 4, 4, 4]
result = 61
)toml";
    auto config = cyane::Config::parse(kBad);
    CYANE_CHECK(config.has_value());
    CraftingRegistry registry;
    const auto rc = registry.load_config(*config);
    CYANE_CHECK(!rc.has_value());
}

CYANE_TEST(toml_table_array_survives_across_sections) {
    // [[recipe]] 与普通 [section] 混合时互不干扰
    constexpr std::string_view kMixed = R"toml(
[server]
port = 25565

[[recipe]]
pattern = [17]
result = 5
)toml";
    auto config = cyane::Config::parse(kMixed);
    CYANE_CHECK(config.has_value());
    CYANE_CHECK_EQ(config->get<std::int64_t>("server.port"), static_cast<std::int64_t>(25565));
    const auto* tables = config->table_array("recipe");
    CYANE_CHECK(tables != nullptr);
    CYANE_CHECK_EQ(tables->size(), static_cast<std::size_t>(1));
}
