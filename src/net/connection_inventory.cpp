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
    auto finish_click = [this, window_id, action, clicked] {
        ByteWriter confirm;
        confirm.u8(*window_id);
        confirm.i16(*action);
        const bool ok = cursor_item_.matches_type(*clicked);
        confirm.boolean(ok);
        send_packet(proto::play_cb::kConfirmTransaction, confirm.data());
        if (!ok) {
            log::warn(
                "{}: window {} cursor mismatch (claimed item {} x{}, now {} x{}) — resyncing window",
                username_, *window_id, (*clicked).id, (*clicked).count, cursor_item_.id,
                cursor_item_.count);
            resync_open_window();
        }
    };
    if (*window_id == kChestWindowId && chest_open_) {
        apply_chest_click(*slot, *button, *mode, *clicked);
        finish_click();
        return true;
    }
    // 小容器（发射器/投掷器/漏斗）与箱子共用点击逻辑，按打开时的容器格数区分
    if (*window_id == kSmallWindowId && small_open_) {
        apply_chest_click(*slot, *button, *mode, *clicked);
        finish_click();
        return true;
    }
    if (*window_id == kFurnaceWindowId && furnace_open_) {
        apply_furnace_click(*slot, *button, *mode, *clicked);
        finish_click();
        return true;
    }
    if (*window_id == kCraftingTableWindowId && table_open_) {
        apply_table_click(*slot, *button, *mode, *clicked);
        finish_click();
        return true;
    }
    // 只处理玩家自身背包 windowId=0；其余窗口本阶段照旧回滚
    if (*window_id == 0) {
        apply_click(*slot, *button, *mode);
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
        finish_click();
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

void Connection::apply_click(std::int16_t slot, std::uint8_t button, std::int32_t mode) {
    // 主背包窗口的槽范围：0..45（slot=-999 表示窗口外，丢弃游标物品）
    const auto slot_count = static_cast<std::int16_t>(item::PlayerInventory::kSlotCount);
    // 合成结果槽 (0)：点击拿取产物并消耗材料，不走普通拿放逻辑
    if (slot == 0 && (mode == 0 || mode == 1)) {
        take_craft_result(mode == 1);
        return;
    }
    // mode 0：普通左/右键拿放；mode 1：shift 快速转移
    if (mode == 1) {
        // shift-move（原版 ContainerPlayer.transferStack 顺序）：
        // 热区栏(36..44) → 主背包；主背包(9..35) → 热区栏（源槽必须在目标区间外，
        // 否则会把整叠合并回自身槽导致物品消失）；合成格/护甲/副手 → 主背包再热区栏
        if (slot < 0 || slot >= slot_count) {
            return;
        }
        item::ItemStack moving = inventory_.slot(static_cast<std::size_t>(slot));
        if (moving.empty()) {
            return;
        }
        if (slot >= 36 && slot <= 44) {
            (void)merge_into_range(moving, 9, 35);
        } else if (slot >= 9 && slot <= 35) {
            (void)merge_into_range(moving, 36, 44);
        } else {
            (void)merge_into_range(moving, 9, 35);
            if (!moving.empty()) {
                (void)merge_into_range(moving, 36, 44);
            }
        }
        inventory_.set_slot(static_cast<std::size_t>(slot), moving);
        send_inventory();
        return;
    }
    if (mode != 0) {
        return;  // 双击/拖拽/数字键等暂不支持
    }
    // 窗口外点击：丢弃游标（本阶段直接清空，掉落物在拾取模块处理）
    if (slot < 0) {
        cursor_item_ = item::ItemStack::air();
        return;
    }
    if (slot >= slot_count) {
        return;
    }
    const std::size_t idx = static_cast<std::size_t>(slot);
    item::ItemStack in_slot = inventory_.slot(idx);
    if (button == 0) {
        // 左键：游标与槽整堆交换，或同类合并
        if (cursor_item_.stacks_with(in_slot)) {
            const int total = static_cast<int>(in_slot.count) + static_cast<int>(cursor_item_.count);
            const int keep = std::min(total, static_cast<int>(item::kMaxStack));
            in_slot.count = static_cast<std::uint8_t>(keep);
            cursor_item_.count = static_cast<std::uint8_t>(total - keep);
            if (cursor_item_.count == 0) {
                cursor_item_ = item::ItemStack::air();
            }
        } else {
            std::swap(cursor_item_, in_slot);
        }
    } else if (button == 1) {
        // 右键：放下一个 / 拿起一半
        if (cursor_item_.empty()) {
            if (!in_slot.empty()) {
                const std::uint8_t take = static_cast<std::uint8_t>((in_slot.count + 1) / 2);
                cursor_item_ = in_slot;
                cursor_item_.count = take;
                in_slot.count = static_cast<std::uint8_t>(in_slot.count - take);
                if (in_slot.count == 0) {
                    in_slot = item::ItemStack::air();
                }
            }
        } else if (in_slot.empty()) {
            in_slot = cursor_item_;
            in_slot.count = 1;
            --cursor_item_.count;
            if (cursor_item_.count == 0) {
                cursor_item_ = item::ItemStack::air();
            }
        } else if (cursor_item_.stacks_with(in_slot) && in_slot.count < item::kMaxStack) {
            ++in_slot.count;
            --cursor_item_.count;
            if (cursor_item_.count == 0) {
                cursor_item_ = item::ItemStack::air();
            }
        } else {
            std::swap(cursor_item_, in_slot);
        }
    } else {
        return;
    }
    inventory_.set_slot(idx, in_slot);
    send_slot(0, slot, in_slot);
    // SetSlot 到 windowId=-1 slot=-1 表示更新游标物品
    send_slot(-1, -1, cursor_item_);
    // 合成格（1..4）变动后重新匹配配方
    if (slot >= 1 && slot <= 4) {
        refresh_crafting_result();
    }
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
                const auto cpos = world::ChunkPos::from_world(
                    static_cast<std::int32_t>(player_pos_.x),
                    static_cast<std::int32_t>(player_pos_.z));
                drop_stack(player_pos_.x, player_pos_.y, player_pos_.z, leftover,
                           cpos ? cpos->x : 0, cpos ? cpos->z : 0);
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
