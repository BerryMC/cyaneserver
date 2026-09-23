#include "cyane/net/connection.hpp"

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
    // 本阶段仅回执并重同步，不做真实拿放。ConfirmTransaction (0x11)：
    // byte windowId | short action | bool accepted。回 accepted=false 让客户端撤销预测。
    ByteWriter confirm;
    confirm.u8(*window_id);
    confirm.i16(*action);
    confirm.boolean(false);
    send_packet(proto::play_cb::kConfirmTransaction, confirm.data());
    // 权威重发被点击槽，客户端据此回滚
    if (*window_id == 0 && *slot >= 0 &&
        *slot < static_cast<std::int16_t>(item::PlayerInventory::kSlotCount)) {
        send_slot(0, *slot, inventory_.slot(static_cast<std::size_t>(*slot)));
    }
    return true;
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
