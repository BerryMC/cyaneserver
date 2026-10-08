#include "cyane/net/connection.hpp"

#include <algorithm>

#include "connection_detail.hpp"

#include "cyane/net/packet_writers.hpp"

namespace cyane::net {

void Connection::encode_dropped_item(const DroppedItem& drop, ByteWriter& out_spawn,
                                     ByteWriter& out_meta) const {
    writers::encode_dropped_item(out_spawn, out_meta, drop.entity_id, drop.x, drop.y, drop.z,
                                 drop.velocity_x, drop.velocity_y, drop.velocity_z, drop.stack);
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

void Connection::collect_items(std::uint64_t now_ms) {
    (void)now_ms;
    if (context_.item_drops == nullptr || dead_) {
        return;
    }
    // 旁观者不拾取物品（vanilla EntityPlayer.onLivingUpdate：isSpectator 跳过碰撞检测）
    if (context_.game_mode == proto::game_mode::kSpectator) {
        return;
    }
    // EntityPlayer.onLivingUpdate：扫描盒 = 玩家包围盒 grow(1.0, 0.5, 1.0)，
    // 物品盒（0.25）相交且 pickupDelay <= 0 → onCollideWithPlayer
    const world::Aabb scan{player_pos_.x - 0.3 - 1.0, player_pos_.y - 0.5,
                           player_pos_.z - 0.3 - 1.0, player_pos_.x + 0.3 + 1.0,
                           player_pos_.y + 1.8 + 0.5, player_pos_.z + 0.3 + 1.0};
    auto picked = context_.item_drops->collect_overlapping(player_id_, scan);
    for (const auto& ev : picked) {
        // 掉落物所在区块（CollectItem/DestroyEntities 只发给附近玩家）
        const auto cpos = world::ChunkPos::from_world(static_cast<std::int32_t>(ev.x),
                                                       static_cast<std::int32_t>(ev.z));
        const std::int32_t radius = std::clamp(context_.view_distance, 2, 8);
        // 物品进背包；放不下的部分留在物品实体上（vanilla：部分拾取不发 CollectItem，
        // 只把剩余堆叠写回并重发 metadata）
        const item::ItemStack leftover = give_item(ev.stack);
        if (leftover.empty()) {
            // 完整拾取：CollectItem (0x4B) + 销毁该掉落物实体
            ByteWriter collect;
            writers::write_collect_item(collect, ev.item_entity_id, ev.collector_id,
                                        ev.stack.count);
            send_packet(proto::play_cb::kCollectItem, collect.data());
            if (context_.hub != nullptr && cpos) {
                context_.hub->broadcast_near(cpos->x, cpos->z, radius, player_id_,
                                             proto::play_cb::kCollectItem, collect.data());
            }
            ByteWriter destroy;
            destroy.varint(1);
            destroy.varint(static_cast<std::int32_t>(ev.item_entity_id));
            send_packet(proto::play_cb::kDestroyEntities, destroy.data());
            if (context_.hub != nullptr && cpos) {
                context_.hub->broadcast_near(cpos->x, cpos->z, radius, player_id_,
                                             proto::play_cb::kDestroyEntities, destroy.data());
            }
        } else {
            context_.item_drops->reduce(ev.item_entity_id, leftover);
            ByteWriter spawn;
            ByteWriter meta;
            DroppedItem view;
            view.entity_id = ev.item_entity_id;
            view.x = ev.x;
            view.y = ev.y;
            view.z = ev.z;
            view.stack = leftover;
            encode_dropped_item(view, spawn, meta);
            send_packet(proto::play_cb::kEntityMetadata, meta.data());
            if (context_.hub != nullptr && cpos) {
                context_.hub->broadcast_near(cpos->x, cpos->z, radius, player_id_,
                                             proto::play_cb::kEntityMetadata, meta.data());
            }
        }
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

}
