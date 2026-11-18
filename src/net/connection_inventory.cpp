#include "cyane/net/connection.hpp"

#include <algorithm>

#include "cyane/core/log.hpp"
#include "cyane/net/packet_writers.hpp"
#include "connection_detail.hpp"
#include "cyane/item/crafting.hpp"

namespace cyane::net {

bool Connection::handle_play_held_item(ByteSpan payload) {
    // HeldItemChange (0x1A)：short slot（0..8）
    ByteReader reader{payload};
    auto slot = reader.i16();
    if (!slot) {
        return false;
    }
    if (*slot >= 0 && *slot < 9) {
        selected_slot_ = static_cast<std::uint8_t>(*slot);
    }
    return true;
}

bool Connection::handle_play_creative_action(ByteSpan payload) {
    // CreativeInventoryAction (0x1B)：short slot | slot 数据
    ByteReader reader{payload};
    auto slot = reader.i16();
    if (!slot) {
        return false;
    }
    auto item = item::read_slot(reader);
    if (!item) {
        return false;
    }
    if (*slot >= 0 && *slot < static_cast<std::int16_t>(item::PlayerInventory::kSlotCount)) {
        inventory_.set_slot(static_cast<std::size_t>(*slot), *item);
        // 合成格变动后重新匹配配方
        if (*slot >= 1 && *slot <= 4) {
            refresh_crafting_result();
        }
    }
    return true;
}

bool Connection::handle_play_click_window(ByteSpan payload) {
    // ClickWindow (0x07)：byte windowId | short slot | byte button | short action
    //                    | varint mode | slot clickedItem
    ByteReader reader{payload};
    auto window_id = reader.u8();
    auto slot = reader.i16();
    auto button = reader.u8();
    auto action = reader.i16();
    auto mode = reader.varint();
    auto clicked = item::read_slot(reader);
    if (!window_id || !slot || !button || !action || !mode || !clicked) {
        return false;
    }
    // 箱子窗口(id=1)交给容器点击处理；熔炉窗口(id=2)交给熔炉处理；windowId=0 走玩家自身背包
    // NMS 游标校验：客户端在包内声明点击后的游标(clickedItem)；物品类型(id+damage)失配时
    // 拒绝事务并全量重同步，防止本地容器状态与服务端持续分叉
    // NetHandlerPlayServer.processClickWindow：用 slotClick 的返回栈与客户端 clickedItem
    // 比对（areItemStacksEqual 含数量），失配则拒绝并全量重同步。
    const auto finish_click = [this, window_id, action, clicked](
                                  const item::ItemStack& expected) {
        ByteWriter confirm;
        confirm.u8(*window_id);
        confirm.i16(*action);
        const bool ok = expected.id == (*clicked).id && expected.damage == (*clicked).damage &&
                        expected.count == (*clicked).count;
        confirm.boolean(ok);
        send_packet(proto::play_cb::kConfirmTransaction, confirm.data());
        if (!ok) {
            log::warn("{}: window {} transaction {} rejected (expected {} x{}, client sent {} x{})",
                      username_, *window_id, *action, expected.id, expected.count, (*clicked).id,
                      (*clicked).count);
            resync_open_window();
        }
    };
    if (*window_id == kChestWindowId && chest_open_) {
        finish_click(apply_chest_click(*slot, *button, *mode, *clicked));
        return true;
    }
    // 小容器（发射器/投掷器/漏斗）与箱子共用点击逻辑，按打开时的容器格数区分
    if (*window_id == kSmallWindowId && small_open_) {
        finish_click(apply_chest_click(*slot, *button, *mode, *clicked));
        return true;
    }
    if (*window_id == kFurnaceWindowId && furnace_open_) {
        // 熔炉槽位尚未改造成 slotClick 语义，仍按游标比对（见 gaps SYS/CONTAINER 待办）
        apply_furnace_click(*slot, *button, *mode, *clicked);
        finish_click(cursor_item_);
        return true;
    }
    if (*window_id == kCraftingTableWindowId && table_open_) {
        apply_table_click(*slot, *button, *mode, *clicked);
        finish_click(cursor_item_);
        return true;
    }
    // 只处理玩家自身背包 windowId=0；其余窗口本阶段照旧回滚
    item::ItemStack expected_click;
    if (*window_id == 0) {
        expected_click = apply_click(*slot, *button, *mode);
    } else if (*window_id == kChestWindowId || *window_id == kFurnaceWindowId ||
               *window_id == kCraftingTableWindowId) {
        // 客户端本地开了窗口、服务端却认为关着（右键未成功开窗等）：点击不会生效，
        // 物品会"回到"背包——记录告警便于定位
        log::warn("{}: window {} click not applied (window not open on server side; "
                  "client may have opened it locally)",
                  username_,
                  *window_id);
    } else {
        // 未知窗口：记录客户端实际发来的窗口号/槽位，便于对齐客户端状态
        log::debug("{}: click on unknown window {} slot {} (ignored)", username_, *window_id, *slot);
    }
    // ConfirmTransaction (0x11)：byte windowId | short action | bool accepted。
    // windowId=0 且游标物品类型失配时拒绝并全量重同步；其余非 0 窗口本阶段不支持，
    // 回 accepted=false 让客户端回滚，并权威重发被点槽。
    if (*window_id == 0) {
        finish_click(expected_click);
    } else {
        ByteWriter confirm;
        confirm.u8(*window_id);
        confirm.i16(*action);
        confirm.boolean(false);
        send_packet(proto::play_cb::kConfirmTransaction, confirm.data());
        if (*slot >= 0 && *slot < static_cast<std::int16_t>(item::PlayerInventory::kSlotCount)) {
            send_slot(0, *slot, inventory_.slot(static_cast<std::size_t>(*slot)));
        }
    }
    return true;
}

bool Connection::merge_into_range(item::ItemStack& moving, std::size_t lo, std::size_t hi) {
    bool changed = false;
    // 先叠到已有同类堆叠
    for (std::size_t i = lo; i <= hi && moving.count > 0; ++i) {
        item::ItemStack dst = inventory_.slot(i);
        if (dst.stacks_with(moving) && dst.count < item::kMaxStack) {
            const int room = item::kMaxStack - dst.count;
            const int take = std::min(room, static_cast<int>(moving.count));
            dst.count = static_cast<std::uint8_t>(dst.count + take);
            moving.count = static_cast<std::uint8_t>(moving.count - take);
            inventory_.set_slot(i, dst);
            changed = true;
        }
    }
    // 再填空槽
    for (std::size_t i = lo; i <= hi && moving.count > 0; ++i) {
        if (inventory_.slot(i).empty()) {
            inventory_.set_slot(i, moving);
            moving = item::ItemStack::air();
            changed = true;
        }
    }
    if (moving.count == 0) {
        moving = item::ItemStack::air();
    }
    return changed;
}

item::ItemStack Connection::apply_click(std::int16_t slot, std::uint8_t button,
                                        std::int32_t mode) {
    // 主背包窗口的槽范围：0..45；-999 表示窗口外
    const auto slot_count = static_cast<std::int16_t>(item::PlayerInventory::kSlotCount);
    // 合成结果槽 (0)：原版 SlotCrafting 只允许取走，放回/其它模式一律忽略
    if (slot == 0) {
        if (mode == click_type::kPickup || mode == click_type::kQuickMove) {
            take_craft_result(mode == click_type::kQuickMove);
        }
        return item::ItemStack::air();
    }

    struct Ops {
        Connection& self;
        std::int16_t slots{0};
        [[nodiscard]] std::int16_t slot_count() const noexcept { return slots; }
        [[nodiscard]] std::uint8_t stack_limit(std::int16_t, const item::ItemStack&) const noexcept {
            return item::kMaxStack;
        }
        [[nodiscard]] item::ItemStack get(std::int16_t s) const {
            return self.inventory_.slot(static_cast<std::size_t>(s));
        }
        void set(std::int16_t s, item::ItemStack stack) {
            self.inventory_.set_slot(static_cast<std::size_t>(s), stack);
        }
        // ContainerPlayer.transferStackInSlot 的分区：热区栏↔主背包，其余→主背包再热区栏
        [[nodiscard]] item::ItemStack transfer(std::int16_t s) {
            auto moving = get(s);
            if (moving.empty()) return moving;
            if (s >= 36 && s <= 44) {
                (void)self.merge_into_range(moving, 9, 35);
            } else if (s >= 9 && s <= 35) {
                (void)self.merge_into_range(moving, 36, 44);
            } else {
                (void)self.merge_into_range(moving, 9, 35);
                if (!moving.empty()) (void)self.merge_into_range(moving, 36, 44);
            }
            set(s, moving);
            return moving;
        }
        void drop(item::ItemStack stack, bool) {
            if (stack.empty()) return;
            const auto [vx, vy, vz] = net::throw_velocity();
            self.drop_stack(self.player_pos_.x, self.player_pos_.y - 0.3 + 1.62,
                            self.player_pos_.z, stack, vx, vy, vz, 40);
        }
        [[nodiscard]] bool add_to_inventory(item::ItemStack stack) {
            return self.give_item(stack).empty();
        }
    };
    Ops ops{*this, slot_count};
    const auto expected = slot_click(ops, cursor_item_, drag_state_, mode, slot,
                                     static_cast<std::int8_t>(button),
                                     context_.game_mode == proto::game_mode::kCreative);

    // 合成格（1..4）变动后重新匹配配方
    if (slot >= 1 && slot <= 4) {
        refresh_crafting_result();
    }
    return expected;
}

void Connection::refresh_crafting_result() {
    if (context_.crafting == nullptr) {
        return;
    }
    std::array<item::ItemStack, 4> cells{};
    for (std::size_t i = 0; i < 4; ++i) {
        cells[i] = inventory_.slot(1 + i);
    }
    inventory_.set_slot(0, item::ItemStack::air());
    const auto recipe = context_.crafting->find_2x2(cells);
    if (recipe) {
        inventory_.set_slot(0, item::ItemStack{recipe->result_id, recipe->result_count, 0});
    }
    send_slot(0, 0, inventory_.slot(0));
}

void Connection::take_craft_result(bool all) {
    // 反复合成直到材料耗尽（all=true 时最多 64 次，防呆）
    for (int round = 0; round < (all ? 64 : 1); ++round) {
        const item::ItemStack result = inventory_.slot(0);
        if (result.empty()) {
            break;
        }
        if (all) {
            // shift：产物直接进背包，放不下的部分掉落在脚下
            const item::ItemStack leftover = give_item(result);
            if (!leftover.empty()) {
                const auto [vx, vy, vz] = net::throw_velocity();
                drop_stack(player_pos_.x, player_pos_.y - 0.3 + 1.62, player_pos_.z, leftover,
                           vx, vy, vz, 40);
            }
        } else {
            // 结果槽产物给游标：仅当游标为空或同类且放得下
            if (!cursor_item_.empty() &&
                (!cursor_item_.stacks_with(result) ||
                 cursor_item_.count + result.count > item::kMaxStack)) {
                break;
            }
            if (cursor_item_.empty()) {
                cursor_item_ = result;
            } else {
                cursor_item_.count = static_cast<std::uint8_t>(cursor_item_.count + result.count);
            }
        }
        // 消耗合成格各 1 个
        for (std::size_t i = 1; i <= 4; ++i) {
            item::ItemStack cell = inventory_.slot(i);
            if (!cell.empty()) {
                --cell.count;
                if (cell.count == 0) {
                    cell = item::ItemStack::air();
                }
                inventory_.set_slot(i, cell);
                send_slot(0, static_cast<std::int16_t>(i), cell);
            }
        }
        refresh_crafting_result();
        if (!all) {
            send_slot(-1, -1, cursor_item_);
        }
    }
}

void Connection::consume_held_item() {
    if (context_.game_mode == proto::game_mode::kCreative) {
        return;
    }
    const std::size_t hs = item::PlayerInventory::hotbar_slot(selected_slot_);
    item::ItemStack after = inventory_.hotbar_item(selected_slot_);
    if (after.count > 0) {
        --after.count;
    }
    if (after.count == 0) {
        after = item::ItemStack::air();
    }
    inventory_.set_slot(hs, after);
    send_slot(0, static_cast<std::int16_t>(hs), after);
}

void Connection::send_slot(std::int8_t window_id, std::int16_t slot, const item::ItemStack& item) {
    // SetSlot (0x16)：byte windowId | short slot | slot data
    ByteWriter fields;
    fields.u8(static_cast<std::uint8_t>(window_id));
    fields.i16(slot);
    item::write_slot(fields, item);
    send_packet(proto::play_cb::kSetSlot, fields.data());
}

void Connection::send_inventory() {
    // WindowItems (0x14)：byte windowId | short count | slot[count]
    ByteWriter fields;
    fields.u8(0);
    fields.i16(static_cast<std::int16_t>(item::PlayerInventory::kSlotCount));
    for (const auto& item : inventory_.slots()) {
        item::write_slot(fields, item);
    }
    send_packet(proto::play_cb::kWindowItems, fields.data());
}

void Connection::resync_open_window() {
    // NMS handleWindowClick 的游标失配修复：重发整窗内容 + 游标，客户端据此对齐本地状态
    if (chest_open_ && context_.containers != nullptr) {
        ByteWriter items;
        items.u8(kChestWindowId);
        items.i16(detail::kChestWindowSlots);
        const auto chest = context_.containers->snapshot(open_chest_key_);
        for (std::int16_t i = 0; i < detail::kChestSlots; ++i) {
            item::write_slot(items, chest[static_cast<std::size_t>(i)]);
        }
        for (std::int16_t i = detail::kChestSlots; i < detail::kChestWindowSlots; ++i) {
            item::write_slot(items, inventory_.slot(detail::container_window_to_player_slot(i,
                                                                                        detail::kChestSlots)));
        }
        send_packet(proto::play_cb::kWindowItems, items.data());
    } else if (small_open_ && context_.containers != nullptr) {
        // 小容器（发射器/投掷器/漏斗）：布局同箱子式，容器格数按打开时的值
        ByteWriter items;
        items.u8(kSmallWindowId);
        const std::int16_t window_slots = static_cast<std::int16_t>(small_slots_ + 36);
        items.i16(window_slots);
        const auto container = context_.containers->snapshot_small(open_small_key_);
        for (std::size_t i = 0; i < small_slots_; ++i) {
            item::write_slot(items, container.slots[i]);
        }
        for (std::int16_t i = static_cast<std::int16_t>(small_slots_); i < window_slots; ++i) {
            item::write_slot(items, inventory_.slot(detail::container_window_to_player_slot(
                                        i, static_cast<std::int16_t>(small_slots_))));
        }
        send_packet(proto::play_cb::kWindowItems, items.data());
    } else if (furnace_open_ && context_.furnaces != nullptr) {
        ByteWriter items;
        items.u8(kFurnaceWindowId);
        items.i16(detail::kFurnaceWindowSlots);
        const auto state = context_.furnaces->snapshot(open_furnace_key_);
        item::write_slot(items, state.input);
        item::write_slot(items, state.fuel);
        item::write_slot(items, state.output);
        for (std::int16_t i = detail::kFurnaceSlots; i < detail::kFurnaceWindowSlots; ++i) {
            item::write_slot(items, inventory_.slot(detail::container_window_to_player_slot(
                                             i, detail::kFurnaceSlots)));
        }
        send_packet(proto::play_cb::kWindowItems, items.data());
    } else if (table_open_) {
        ByteWriter items;
        items.u8(kCraftingTableWindowId);
        items.i16(detail::kTableSlots + 36);
        for (const auto& cell : table_grid_) {
            item::write_slot(items, cell);
        }
        item::write_slot(items, table_result_);
        for (std::size_t idx = 9; idx < item::PlayerInventory::kSlotCount; ++idx) {
            item::write_slot(items, inventory_.slot(idx));
        }
        send_packet(proto::play_cb::kWindowItems, items.data());
    } else {
        send_inventory();
    }
    send_slot(-1, -1, cursor_item_);
}

void Connection::send_spawn_player() {
    ByteWriter info;
    detail::write_player_info_add(info, uuid_.bytes(), username_, context_.game_mode);
    send_packet(proto::play_cb::kPlayerInfo, info.data());
}

void Connection::broadcast_spawn() {
    if (context_.hub == nullptr) {
        return;
    }
    ByteWriter info;
    detail::write_player_info_add(info, uuid_.bytes(), username_, context_.game_mode);
    context_.hub->broadcast(player_id_, proto::play_cb::kPlayerInfo, info.data());

    // 旁观者不向其他人发送 SpawnPlayer（原版 isSpectatedByPlayer 返回 false）
    if (context_.game_mode == proto::game_mode::kSpectator) {
        return;
    }
    ByteWriter spawn;
    detail::write_named_spawn(spawn, player_id_, uuid_.bytes(), player_pos_.x, player_pos_.y,
                              player_pos_.z, player_pos_.yaw, player_pos_.pitch);
    context_.hub->broadcast(player_id_, proto::play_cb::kSpawnPlayer, spawn.data());
}

void Connection::spawn_existing_players() {
    if (context_.hub == nullptr) {
        return;
    }
    for (const auto& other : context_.hub->others(player_id_)) {
        ByteWriter info;
        detail::write_player_info_add(info, other.uuid, other.name, other.game_mode);
        send_packet(proto::play_cb::kPlayerInfo, info.data());

        // 旁观者不向其他人发送 SpawnPlayer（原版 isSpectatedByPlayer 返回 false）
        if (other.game_mode == proto::game_mode::kSpectator) {
            continue;
        }
        ByteWriter spawn;
        detail::write_named_spawn(spawn, other.entity_id, other.uuid, other.x, other.y, other.z,
                                  other.yaw, other.pitch);
        send_packet(proto::play_cb::kSpawnPlayer, spawn.data());
    }
}

void Connection::broadcast_despawn() {
    if (context_.hub == nullptr || player_id_ == 0) {
        return;
    }
    ByteWriter destroy;
    {
        const std::uint32_t ids[] = {player_id_};
        writers::write_destroy_entities(destroy, ids);
    }
    context_.hub->broadcast(player_id_, proto::play_cb::kDestroyEntities, destroy.data());
    // PlayerInfo(remove=4)：count | uuid(16)
    ByteWriter info;
    info.varint(proto::play_cb::kPlayerInfoRemovePlayer);
    info.varint(1);
    info.bytes(ByteSpan{reinterpret_cast<const std::byte*>(uuid_.bytes().data()), uuid_.bytes().size()});
    context_.hub->broadcast(player_id_, proto::play_cb::kPlayerInfo, info.data());

    context_.hub->unregister_player(player_id_);
    hub_entry_.reset();
}

void Connection::register_in_hub() {
    if (context_.hub == nullptr) {
        return;
    }
    PlayerSnapshot snap;
    snap.entity_id = player_id_;
    snap.uuid = uuid_.bytes();
    snap.name = username_;
    snap.x = player_pos_.x;
    snap.y = player_pos_.y;
    snap.z = player_pos_.z;
    snap.yaw = player_pos_.yaw;
    snap.pitch = player_pos_.pitch;
    snap.game_mode = context_.game_mode;
    hub_entry_ = context_.hub->register_player(snap);
    spawn_existing_players();
    broadcast_spawn();
}

}
