#include "cyane/net/connection.hpp"

#include <algorithm>

#include "connection_detail.hpp"

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
    // 箱子窗口(id=1)交给容器点击处理；windowId=0 走玩家自身背包
    if (*window_id == kChestWindowId && chest_open_) {
        apply_chest_click(*slot, *button, *mode);
        ByteWriter confirm;
        confirm.u8(*window_id);
        confirm.i16(*action);
        confirm.boolean(true);
        send_packet(proto::play_cb::kConfirmTransaction, confirm.data());
        return true;
    }
    // 只处理玩家自身背包 windowId=0；其余窗口本阶段照旧回滚
    if (*window_id == 0) {
        apply_click(*slot, *button, *mode);
    }
    // ConfirmTransaction (0x11)：byte windowId | short action | bool accepted。
    // 服务端已按权威状态处理，回 accepted=true 表示接受该事务。
    ByteWriter confirm;
    confirm.u8(*window_id);
    confirm.i16(*action);
    confirm.boolean(*window_id == 0);
    send_packet(proto::play_cb::kConfirmTransaction, confirm.data());
    // windowId≠0 的窗口本阶段不支持：权威重发被点槽让客户端回滚
    if (*window_id != 0 && *slot >= 0 &&
        *slot < static_cast<std::int16_t>(item::PlayerInventory::kSlotCount)) {
        send_slot(0, *slot, inventory_.slot(static_cast<std::size_t>(*slot)));
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
    // mode 0：普通左/右键拿放；mode 1：shift 快速转移
    if (mode == 1) {
        // shift-move：热区栏(36..44) ↔ 主背包(9..35) 之间整堆转移
        if (slot < 0 || slot >= slot_count) {
            return;
        }
        item::ItemStack moving = inventory_.slot(static_cast<std::size_t>(slot));
        if (moving.empty()) {
            return;
        }
        const bool from_hotbar = slot >= 36 && slot <= 44;
        const std::size_t lo = from_hotbar ? 9 : 36;
        const std::size_t hi = from_hotbar ? 35 : 44;
        if (merge_into_range(moving, lo, hi)) {
            inventory_.set_slot(static_cast<std::size_t>(slot), moving);
            send_inventory();
        }
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

void Connection::send_spawn_player() {
    ByteWriter info;
    detail::write_player_info_add(info, uuid_bytes_, username_);
    send_packet(proto::play_cb::kPlayerInfo, info.data());
}

void Connection::broadcast_spawn() {
    if (context_.hub == nullptr) {
        return;
    }
    ByteWriter info;
    detail::write_player_info_add(info, uuid_bytes_, username_);
    context_.hub->broadcast(player_id_, proto::play_cb::kPlayerInfo, info.data());

    ByteWriter spawn;
    detail::write_named_spawn(spawn, player_id_, uuid_bytes_, player_pos_.x, player_pos_.y,
                              player_pos_.z, player_pos_.yaw, player_pos_.pitch);
    context_.hub->broadcast(player_id_, proto::play_cb::kSpawnPlayer, spawn.data());
}

void Connection::spawn_existing_players() {
    if (context_.hub == nullptr) {
        return;
    }
    for (const auto& other : context_.hub->others(player_id_)) {
        ByteWriter info;
        detail::write_player_info_add(info, other.uuid, other.name);
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
    // DestroyEntities (0x32)：varint count | varint[] ids
    ByteWriter destroy;
    destroy.varint(1);
    destroy.varint(static_cast<std::int32_t>(player_id_));
    context_.hub->broadcast(player_id_, proto::play_cb::kDestroyEntities, destroy.data());
    // PlayerInfo(remove=4)：count | uuid(16)
    ByteWriter info;
    info.varint(proto::play_cb::kPlayerInfoRemovePlayer);
    info.varint(1);
    info.bytes(ByteSpan{reinterpret_cast<const std::byte*>(uuid_bytes_.data()), uuid_bytes_.size()});
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
    snap.uuid = uuid_bytes_;
    snap.name = username_;
    snap.x = player_pos_.x;
    snap.y = player_pos_.y;
    snap.z = player_pos_.z;
    snap.yaw = player_pos_.yaw;
    snap.pitch = player_pos_.pitch;
    hub_entry_ = context_.hub->register_player(snap);
    spawn_existing_players();
    broadcast_spawn();
}

}
