#include "cyane/net/connection.hpp"

#include <algorithm>

#include "connection_detail.hpp"

#include "cyane/net/packet_writers.hpp"

namespace cyane::net {

namespace {
// 1.12.2 EntityItem 的元数据：index 6 = 物品堆叠，type 5 = Slot
constexpr std::uint8_t kItemMetaIndex = 6;
constexpr std::uint8_t kMetaTypeSlot = 6;  // 1.11+ meta 类型表：Slot=6（1.8-1.10 是 5）
constexpr std::int32_t kObjectTypeItem = 2;  // SpawnObject type：掉落物
}

void Connection::encode_dropped_item(const DroppedItem& drop, ByteWriter& out_spawn,
                                     ByteWriter& out_meta) const {
    // SpawnObject (0x00)：varint id | uuid(16) | byte type | double x/y/z
    //                    | byte pitch | byte yaw | int data | short vX/vY/vZ
    out_spawn.varint(static_cast<std::int32_t>(drop.entity_id));
    std::array<std::uint8_t, 16> uuid{};
    uuid[15] = static_cast<std::uint8_t>(drop.entity_id & 0xFF);
    uuid[14] = static_cast<std::uint8_t>((drop.entity_id >> 8) & 0xFF);
    out_spawn.bytes(ByteSpan{reinterpret_cast<const std::byte*>(uuid.data()), uuid.size()});
    out_spawn.u8(static_cast<std::uint8_t>(kObjectTypeItem));
    out_spawn.f64(drop.x);
    out_spawn.f64(drop.y);
    out_spawn.f64(drop.z);
    out_spawn.u8(0);   // pitch
    out_spawn.u8(0);   // yaw
    out_spawn.i32(1);  // data≠0：让客户端读取后续速度
    out_spawn.i16(0);
    out_spawn.i16(0);
    out_spawn.i16(0);

    // EntityMetadata (0x3C)：varint id | (byte index | varint type | 值)... | 0xFF 终止
    // 只写 index 6 的物品堆叠，客户端据此把实体渲染成对应物品
    out_meta.varint(static_cast<std::int32_t>(drop.entity_id));
    out_meta.u8(kItemMetaIndex);
    out_meta.varint(kMetaTypeSlot);
    item::write_slot(out_meta, drop.stack);
    out_meta.u8(0xFF);
}

void Connection::spawn_dropped_item(const DroppedItem& drop) {
    ByteWriter spawn;
    ByteWriter meta;
    encode_dropped_item(drop, spawn, meta);
    send_packet(proto::play_cb::kSpawnObject, spawn.data());
    send_packet(proto::play_cb::kEntityMetadata, meta.data());
}

void Connection::send_existing_drops() {
    if (context_.item_drops == nullptr) {
        return;
    }
    for (const auto& drop : context_.item_drops->snapshot()) {
        spawn_dropped_item(drop);
    }
}

item::ItemStack Connection::give_item(item::ItemStack stack) {
    using Inv = item::PlayerInventory;
    // 先叠到已有同类堆叠（热区栏 36..44，再主背包 9..35），再填空槽
    const std::size_t order_lo[] = {Inv::kHotbarStart, 9};
    const std::size_t order_hi[] = {Inv::kHotbarStart + Inv::kHotbarCount - 1, 35};
    for (int phase = 0; phase < 2 && stack.count > 0; ++phase) {
        for (int pass = 0; pass < 2 && stack.count > 0; ++pass) {
            for (std::size_t i = order_lo[phase]; i <= order_hi[phase] && stack.count > 0; ++i) {
                item::ItemStack dst = inventory_.slot(i);
                if (pass == 0) {
                    if (dst.stacks_with(stack) && dst.count < item::kMaxStack) {
                        const int room = item::kMaxStack - dst.count;
                        const int take = std::min(room, static_cast<int>(stack.count));
                        dst.count = static_cast<std::uint8_t>(dst.count + take);
                        stack.count = static_cast<std::uint8_t>(stack.count - take);
                        inventory_.set_slot(i, dst);
                        send_slot(0, static_cast<std::int16_t>(i), dst);
                    }
                } else if (dst.empty()) {
                    inventory_.set_slot(i, stack);
                    send_slot(0, static_cast<std::int16_t>(i), stack);
                    stack = item::ItemStack::air();
                }
            }
        }
    }
    return stack;
}

void Connection::collect_items(std::uint64_t now_ms) {
    if (context_.item_drops == nullptr || dead_) {
        return;
    }
    auto picked = context_.item_drops->collect_near(player_id_, player_pos_.x, player_pos_.y,
                                                    player_pos_.z, now_ms);
    for (const auto& ev : picked) {
        // 掉落物所在区块（CollectItem/DestroyEntities 只发给附近玩家）
        const auto cpos = world::ChunkPos::from_world(static_cast<std::int32_t>(ev.x),
                                                       static_cast<std::int32_t>(ev.z));
        const std::int32_t radius = std::clamp(context_.view_distance, 2, 8);
        // CollectItem (0x4B)：varint collectedId | varint collectorId | varint count
        ByteWriter collect;
        writers::write_collect_item(collect, ev.item_entity_id, ev.collector_id, ev.stack.count);
        send_packet(proto::play_cb::kCollectItem, collect.data());
        if (context_.hub != nullptr && cpos) {
            context_.hub->broadcast_near(cpos->x, cpos->z, radius, player_id_,
                                         proto::play_cb::kCollectItem, collect.data());
        }
        // 物品进背包；放不下的部分回吐为新的掉落物（就在玩家脚下）
        const item::ItemStack leftover = give_item(ev.stack);
        if (!leftover.empty()) {
            const auto pcpos = world::ChunkPos::from_world(static_cast<std::int32_t>(player_pos_.x),
                                                            static_cast<std::int32_t>(player_pos_.z));
            drop_stack(player_pos_.x, player_pos_.y, player_pos_.z, leftover,
                       pcpos ? pcpos->x : 0, pcpos ? pcpos->z : 0);
        }
        // 销毁该掉落物实体
        ByteWriter destroy;
        destroy.varint(1);
        destroy.varint(static_cast<std::int32_t>(ev.item_entity_id));
        send_packet(proto::play_cb::kDestroyEntities, destroy.data());
        if (context_.hub != nullptr && cpos) {
            context_.hub->broadcast_near(cpos->x, cpos->z, radius, player_id_,
                                         proto::play_cb::kDestroyEntities, destroy.data());
        }
    }
}

}
