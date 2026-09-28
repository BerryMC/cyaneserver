#include "cyane/net/furnace_store.hpp"
#include "test_framework.hpp"

using cyane::net::FurnaceStore;
using cyane::item::ItemStack;

namespace {
constexpr std::int16_t kIronOre = 15;
constexpr std::int16_t kIronIngot = 265;
constexpr std::int16_t kCoal = 263;

// FurnaceStore 含 mutex 不可拷贝/移动，就地构造并注入数据表
void init_store(FurnaceStore& store) {
    FurnaceStore::FuelMap fuel{{kCoal, 1600}};
    FurnaceStore::SmeltMap smelting{{kIronOre, {kIronIngot, 1}}};
    store.set_tables(std::move(fuel), std::move(smelting));
}
}  // namespace

CYANE_TEST(furnace_ignites_fuel_and_smelts) {
    FurnaceStore store;
    init_store(store);
    const std::int64_t key = 1;
    store.ensure(key);
    store.set_slot(key, 0, ItemStack{kIronOre, 1, 0});
    store.set_slot(key, 1, ItemStack{kCoal, 1, 0});

    // 点火瞬间：点火与燃烧同 tick 发生，剩余 1599，燃烧总值 1600
    store.tick();
    auto state = store.snapshot(key);
    CYANE_CHECK_EQ(state.burn_left, 1599);
    CYANE_CHECK_EQ(state.burn_total, 1600);
    // 燃料被消耗 1
    CYANE_CHECK_EQ(state.fuel.count, static_cast<std::uint8_t>(0));

    // 冶炼 200 tick 后产出铁锭，输入消耗
    for (int i = 0; i < 200; ++i) {
        store.tick();
    }
    state = store.snapshot(key);
    CYANE_CHECK_EQ(state.output.id, kIronIngot);
    CYANE_CHECK_EQ(state.output.count, static_cast<std::uint8_t>(1));
    CYANE_CHECK(state.input.empty());
    // 进度归零
    CYANE_CHECK_EQ(state.cook_time, 0);
}

CYANE_TEST(furnace_wont_ignite_without_valid_input) {
    FurnaceStore store;
    init_store(store);
    const std::int64_t key = 1;
    store.ensure(key);
    // 只有燃料没有可冶炼输入
    store.set_slot(key, 1, ItemStack{kCoal, 1, 0});
    store.tick();
    CYANE_CHECK_EQ(store.snapshot(key).burn_left, 0);
    // 燃料未被消耗
    CYANE_CHECK_EQ(store.snapshot(key).fuel.count, static_cast<std::uint8_t>(1));
}

CYANE_TEST(furnace_cook_time_decays_when_input_removed) {
    FurnaceStore store;
    init_store(store);
    const std::int64_t key = 1;
    store.ensure(key);
    store.set_slot(key, 0, ItemStack{kIronOre, 2, 0});
    store.set_slot(key, 1, ItemStack{kCoal, 1, 0});
    // 推进 100 tick
    for (int i = 0; i < 100; ++i) {
        store.tick();
    }
    CYANE_CHECK_EQ(store.snapshot(key).cook_time, 100);
    // 拿走输入
    store.set_slot(key, 0, ItemStack::air());
    store.tick();
    CYANE_CHECK_EQ(store.snapshot(key).cook_time, 99);
}

CYANE_TEST(furnace_output_accumulates_and_blocks_when_full) {
    FurnaceStore store;
    init_store(store);
    const std::int64_t key = 1;
    store.ensure(key);
    store.set_slot(key, 0, ItemStack{kIronOre, 64, 0});
    store.set_slot(key, 1, ItemStack{kCoal, 64, 0});
    // 冶炼 63 个（63*200 tick）
    for (int i = 0; i < 63 * 200; ++i) {
        store.tick();
    }
    auto state = store.snapshot(key);
    CYANE_CHECK_EQ(state.output.id, kIronIngot);
    CYANE_CHECK_EQ(state.output.count, static_cast<std::uint8_t>(63));
    // 再冶炼 1 个到 64，输入耗尽
    for (int i = 0; i < 200; ++i) {
        store.tick();
    }
    CYANE_CHECK_EQ(store.snapshot(key).output.count, static_cast<std::uint8_t>(64));
    CYANE_CHECK_EQ(store.snapshot(key).input.count, static_cast<std::uint8_t>(0));
    // 产物已满：再放一个输入也不冶炼，进度停在 0
    store.set_slot(key, 0, ItemStack{kIronOre, 1, 0});
    for (int i = 0; i < 200; ++i) {
        store.tick();
    }
    CYANE_CHECK_EQ(store.snapshot(key).output.count, static_cast<std::uint8_t>(64));
    CYANE_CHECK_EQ(store.snapshot(key).input.count, static_cast<std::uint8_t>(1));
    CYANE_CHECK_EQ(store.snapshot(key).cook_time, 0);
}
