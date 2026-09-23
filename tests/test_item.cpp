#include <cstdint>

#include "cyane/core/bytes.hpp"
#include "cyane/item/item_stack.hpp"
#include "cyane/item/player_inventory.hpp"
#include "cyane/world/blocks.hpp"
#include "test_framework.hpp"

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

