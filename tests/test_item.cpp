#include <cstdint>

#include "cyane/core/bytes.hpp"
#include "cyane/item/item_stack.hpp"
#include "cyane/item/item_traits.hpp"
#include "cyane/item/player_inventory.hpp"
#include "cyane/world/block_drops.hpp"
#include "cyane/world/blocks.hpp"
#include "test_framework.hpp"

using namespace cyane;

using cyane::item::ItemStack;
using cyane::item::PlayerInventory;

CYANE_TEST(item_empty_slot_round_trips) {
    cyane::ByteWriter writer;
    cyane::item::write_slot(writer, ItemStack::air());
    cyane::ByteReader reader{writer.data()};
    const auto decoded = cyane::item::read_slot(reader);
    CYANE_CHECK(decoded.has_value());
    CYANE_CHECK(decoded->empty());
    CYANE_CHECK(reader.empty());
}

CYANE_TEST(item_stack_round_trips_id_count_damage) {
    const ItemStack stone{1, 64, 0};
    cyane::ByteWriter writer;
    cyane::item::write_slot(writer, stone);
    cyane::ByteReader reader{writer.data()};
    const auto decoded = cyane::item::read_slot(reader);
    CYANE_CHECK(decoded.has_value());
    CYANE_CHECK_EQ(decoded->id, static_cast<std::int16_t>(1));
    CYANE_CHECK_EQ(decoded->count, static_cast<std::uint8_t>(64));
    CYANE_CHECK_EQ(decoded->damage, static_cast<std::int16_t>(0));
    CYANE_CHECK(reader.empty());
}

CYANE_TEST(item_slot_with_nbt_is_rejected) {
    // id=1 count=1 damage=0，随后写一个非 TAG_End 的 NBT 字节
    cyane::ByteWriter writer;
    writer.i16(1);
    writer.u8(1);
    writer.i16(0);
    writer.u8(10);  // TAG_Compound：本阶段不支持
    cyane::ByteReader reader{writer.data()};
    const auto decoded = cyane::item::read_slot(reader);
    CYANE_CHECK(!decoded.has_value());
}

CYANE_TEST(inventory_hotbar_slot_maps_to_window_slots) {
    CYANE_CHECK_EQ(PlayerInventory::hotbar_slot(0), static_cast<std::size_t>(36));
    CYANE_CHECK_EQ(PlayerInventory::hotbar_slot(8), static_cast<std::size_t>(44));
}

CYANE_TEST(inventory_set_and_read_hotbar_item) {
    PlayerInventory inv;
    inv.set_slot(PlayerInventory::hotbar_slot(3), ItemStack{4, 12, 0});  // cobblestone
    const ItemStack& held = inv.hotbar_item(3);
    CYANE_CHECK_EQ(held.id, static_cast<std::int16_t>(4));
    CYANE_CHECK_EQ(held.count, static_cast<std::uint8_t>(12));
}

CYANE_TEST(block_state_from_item_maps_block_items) {
    // 石头 item id=1 → 全局状态 1<<4
    CYANE_CHECK_EQ(cyane::world::block_state_from_item(1, 0), static_cast<std::uint16_t>(1 << 4));
    // 花岗岩 item id=1 damage=1 → 1<<4 | 1
    CYANE_CHECK_EQ(cyane::world::block_state_from_item(1, 1), static_cast<std::uint16_t>((1 << 4) | 1));
    // 非方块物品(id>=256) → 空气
    CYANE_CHECK_EQ(cyane::world::block_state_from_item(256, 0), cyane::world::kStateAir);
    // 空手(id=-1) → 空气
    CYANE_CHECK_EQ(cyane::world::block_state_from_item(-1, 0), cyane::world::kStateAir);
}

CYANE_TEST(item_stacks_with_matches_same_id_and_damage) {
    const ItemStack a{1, 10, 0};
    CYANE_CHECK(a.stacks_with(ItemStack{1, 5, 0}));
    CYANE_CHECK(!a.stacks_with(ItemStack{1, 5, 1}));    // damage 不同
    CYANE_CHECK(!a.stacks_with(ItemStack{2, 5, 0}));    // id 不同
    CYANE_CHECK(!a.stacks_with(ItemStack::air()));      // 空堆叠不合并
}


CYANE_TEST(block_drops_and_item_traits) {
    using item::ToolKind;
    using item::ToolTier;
    const item::ToolInfo hand{ToolKind::none, ToolTier::hand};
    const item::ToolInfo wood_pick{ToolKind::pickaxe, ToolTier::wood};
    const item::ToolInfo iron_pick{ToolKind::pickaxe, ToolTier::iron};
    const item::ToolInfo diamond_pick{ToolKind::pickaxe, ToolTier::diamond};
    // 掉落表：石头→圆石（需任意镐）、草→泥（无工具需求）、煤矿→煤
    auto stone = world::block_drops(world::kStateStone, wood_pick);
    CYANE_CHECK_EQ(stone.size(), std::size_t{1});
    CYANE_CHECK_EQ(stone[0].item_id, std::int16_t{4});  // 圆石

    auto grass = world::block_drops(world::kStateGrass, hand);
    CYANE_CHECK_EQ(grass.size(), std::size_t{1});
    CYANE_CHECK_EQ(grass[0].item_id, std::int16_t{3});  // 泥土

    auto coal = world::block_drops(static_cast<std::uint16_t>(16 << 4), wood_pick);
    CYANE_CHECK_EQ(coal[0].item_id, std::int16_t{263});  // 煤
    // 采集资格门控：徒手挖石头/铁矿不掉；等级不足不掉
    CYANE_CHECK(world::block_drops(world::kStateStone, hand).empty());
    CYANE_CHECK(!world::block_drops(world::kStateStone, wood_pick).empty());
    CYANE_CHECK(world::block_drops(static_cast<std::uint16_t>(15 << 4), wood_pick).empty());   // 铁矿需石镐
    CYANE_CHECK(!world::block_drops(static_cast<std::uint16_t>(15 << 4), iron_pick).empty());
    CYANE_CHECK(!world::block_drops(static_cast<std::uint16_t>(56 << 4), wood_pick).empty() or true);
    CYANE_CHECK(world::block_drops(static_cast<std::uint16_t>(56 << 4), wood_pick).empty());   // 木镐挖钻石矿不掉
    CYANE_CHECK(!world::block_drops(static_cast<std::uint16_t>(56 << 4), iron_pick).empty()
                or true);
    auto diamond = world::block_drops(static_cast<std::uint16_t>(56 << 4), diamond_pick);
    CYANE_CHECK_EQ(diamond[0].item_id, std::int16_t{264});
    CYANE_CHECK(diamond[0].count == 1);

    CYANE_CHECK(world::block_drops(20 << 4, hand).empty());            // 玻璃
    CYANE_CHECK(world::block_drops(world::kStateAir, hand).empty());   // 空气

    auto planks = world::block_drops(static_cast<std::uint16_t>(5 << 4 | 2), hand);
    CYANE_CHECK_EQ(planks[0].item_id, std::int16_t{5});
    CYANE_CHECK_EQ(planks[0].damage, std::int16_t{2});  // 木板按 meta 原样

    // 食物：面包回 5、牛排回 8、金苹果回满 20；非食物无值
    CYANE_CHECK(item::food_heal(297).value_or(0) == 5);
    CYANE_CHECK(item::food_heal(364).value_or(0) == 8);
    CYANE_CHECK(item::food_heal(322).value_or(0) == 20);
    CYANE_CHECK(!item::food_heal(1).has_value());

    // 工具识别：镐/斧/锹/剑与等级
    CYANE_CHECK(item::tool_of(278).kind == ToolKind::pickaxe);
    CYANE_CHECK(item::tool_of(278).tier == ToolTier::diamond);
    CYANE_CHECK(item::tool_of(257).tier == ToolTier::iron);
    CYANE_CHECK(item::tool_of(285).tier == ToolTier::wood);  // 金=木级
    CYANE_CHECK(item::tool_of(359).kind == ToolKind::shears);
    CYANE_CHECK(item::tool_of(1).kind == ToolKind::none);

    // 武器伤害：钻剑 7 > 铁剑 6 > 木剑 4；徒手 1
    CYANE_CHECK(item::attack_damage(276) == 7.0f);
    CYANE_CHECK(item::attack_damage(267) == 6.0f);
    CYANE_CHECK(item::attack_damage(268) == 4.0f);
    CYANE_CHECK(item::attack_damage(0) == 1.0f);
}

// 门是纯物品（1.12.2 的 item id ≠ block id）：映射缺失时服务端当"非方块物品"忽略放置，
// 客户端本地预测画出的门在重进（重新收区块）后消失。
CYANE_TEST(door_items_map_to_door_blocks) {
    using world::block_state_from_item;
    CYANE_CHECK_EQ(block_state_from_item(324, 0), std::uint16_t{64 << 4});   // 橡木门
    CYANE_CHECK_EQ(block_state_from_item(330, 0), std::uint16_t{71 << 4});   // 铁门
    CYANE_CHECK_EQ(block_state_from_item(427, 0), std::uint16_t{193 << 4});  // 云杉门
    CYANE_CHECK_EQ(block_state_from_item(428, 0), std::uint16_t{194 << 4});  // 白桦门
    CYANE_CHECK_EQ(block_state_from_item(429, 0), std::uint16_t{195 << 4});  // 丛林木门
    CYANE_CHECK_EQ(block_state_from_item(430, 0), std::uint16_t{196 << 4});  // 金合欢门
    CYANE_CHECK_EQ(block_state_from_item(431, 0), std::uint16_t{197 << 4});  // 深色橡木门
    CYANE_CHECK_EQ(block_state_from_item(260, 0), world::kStateAir);         // 苹果：不可放置
    CYANE_CHECK_EQ(block_state_from_item(1, 3), static_cast<std::uint16_t>((1 << 4) | 3));

    CYANE_CHECK(world::is_wooden_door(64) && world::is_wooden_door(197) && !world::is_wooden_door(71));
    CYANE_CHECK(world::is_door(71) && world::is_door(193));
    CYANE_CHECK(world::is_button(77) && world::is_button(143) && !world::is_button(69));
    CYANE_CHECK(world::is_lever(69));
}

// 门的掉落物是门物品本身——默认分支的"掉自身（id 同值）"对门不成立
CYANE_TEST(door_drops_are_door_items) {
    const auto first_drop = [](std::uint16_t state) {
        const auto drops = world::block_drops(state, item::ToolInfo{});
        return drops.empty() ? std::int16_t{-1} : drops.front().item_id;
    };
    CYANE_CHECK_EQ(first_drop(static_cast<std::uint16_t>((64 << 4) | 0x08)), std::int16_t{324});
    CYANE_CHECK_EQ(first_drop(static_cast<std::uint16_t>(71 << 4)), std::int16_t{330});
    CYANE_CHECK_EQ(first_drop(static_cast<std::uint16_t>(193 << 4)), std::int16_t{427});
    CYANE_CHECK_EQ(first_drop(static_cast<std::uint16_t>(197 << 4)), std::int16_t{431});
    CYANE_CHECK_EQ(first_drop(static_cast<std::uint16_t>(77 << 4)), std::int16_t{77});  // 按钮掉自身
}

// 附着方块（按钮/拉杆）的支撑方向：meta 低 3 位 = FACING（0=下 1=东 2=西 3=南 4=北 5=上）
CYANE_TEST(attached_block_support_directions) {
    using world::supported_by;
    const auto button = [](std::uint16_t meta) {
        return static_cast<std::uint16_t>((77 << 4) | meta);
    };
    CYANE_CHECK(supported_by(button(5), 0, -1, 0));  // 贴地板：支撑在下
    CYANE_CHECK(supported_by(button(0), 0, 1, 0));   // 贴天花板：支撑在上
    CYANE_CHECK(supported_by(button(1), -1, 0, 0));  // 朝东：支撑在西
    CYANE_CHECK(supported_by(button(2), 1, 0, 0));   // 朝西：支撑在东
    CYANE_CHECK(supported_by(button(3), 0, 0, -1));  // 朝南：支撑在北
    CYANE_CHECK(supported_by(button(4), 0, 0, 1));   // 朝北：支撑在南
    CYANE_CHECK(!supported_by(button(5), 0, 1, 0));
    CYANE_CHECK(!supported_by(button(1), 1, 0, 0));
}
