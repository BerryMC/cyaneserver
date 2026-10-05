#include "cyane/net/connection.hpp"

#include <algorithm>
#include <array>

#include "cyane/item/item_stack.hpp"
#include "cyane/net/item_drop.hpp"
#include "cyane/net/mob_manager.hpp"
#include "cyane/net/packet_writers.hpp"

namespace cyane::net {
namespace {

// 出生 metadata：客户端只在出生包注册 watcher 条目，后续对未注册 index 的增量会被忽略。
// 索引按 DataWatcher defineId 的父类链计数分配（Forge 反混淆源码核实）：
// Entity 0-5 + EntityLivingBase 6-10 + EntityLiving(AI_FLAGS) 11 → 各物种自己的字段从 12 起。
//   苦力怕 STATE = 12（VARINT 序 1，-1 空闲/1 引信中）；骷髅 SWINGING_ARMS = 12（BOOLEAN 序 7）。
void encode_spawn_mob(ByteWriter& out, const Mob& mob) {
    ByteWriter meta;
    if (mob.type == 50) {
        meta.u8(12);
        meta.varint(1);
        meta.varint(-1);
    } else if (mob.type == 51) {
        meta.u8(12);
        meta.varint(6);  // DataSerializers.BOOLEAN（Forge 注册序：BYTE,VARINT,FLOAT,STRING,TEXT,ITEM_STACK,BOOLEAN）
        meta.u8(0);
    }
    writers::write_spawn_mob(out, mob.entity_id, mob.type, mob.pos.x, mob.pos.y, mob.pos.z,
                             mob.pos.yaw, meta.data());
}

// 骷髅主手拿弓：EntityEquipment (0x3F) varint id | varint slot 0(主手) | slot
void encode_skeleton_bow(ByteWriter& out, std::uint32_t entity_id) {
    out.varint(static_cast<std::int32_t>(entity_id));
    out.varint(0);
    item::write_slot(out, item::ItemStack{261, 1, 0});
}

}  // namespace

bool Connection::spawn_mob_at(std::int32_t type, double x, double y, double z, float yaw) {
    if (context_.mobs == nullptr) {
        return false;
    }
    const auto id = context_.mobs->spawn(type, x, y, z, yaw);
    if (id == 0) {
        return false;
    }
    const auto mob = context_.mobs->by_id(id);
    if (!mob) {
        return false;
    }
    ByteWriter spawn;
    encode_spawn_mob(spawn, *mob);
    send_packet(proto::play_cb::kSpawnMob, spawn.data());
    ByteWriter equipment;
    if (mob->type == 51) {
        encode_skeleton_bow(equipment, mob->entity_id);
        send_packet(proto::play_cb::kEntityEquipment, equipment.data());
    }
    if (context_.hub != nullptr) {
        if (const auto cpos = world::ChunkPos::from_world(static_cast<std::int32_t>(x),
                                                           static_cast<std::int32_t>(z))) {
            const std::int32_t radius = std::clamp(context_.view_distance, 2, 8);
            context_.hub->broadcast_near(cpos->x, cpos->z, radius, player_id_,
                                         proto::play_cb::kSpawnMob, spawn.data());
            if (mob->type == 51) {
                context_.hub->broadcast_near(cpos->x, cpos->z, radius, player_id_,
                                             proto::play_cb::kEntityEquipment, equipment.data());
            }
        }
    }
    return true;
}

void Connection::send_chunk_entities(std::int32_t cx, std::int32_t cz) {
    auto in_chunk = [](double x, double z, std::int32_t cx, std::int32_t cz) {
        return static_cast<std::int32_t>(std::floor(x / 16.0)) == cx &&
               static_cast<std::int32_t>(std::floor(z / 16.0)) == cz;
    };
    if (context_.mobs != nullptr) {
        for (const auto& mob : context_.mobs->snapshot()) {
            if (!in_chunk(mob.pos.x, mob.pos.z, cx, cz)) {
                continue;
            }
            ByteWriter spawn;
            encode_spawn_mob(spawn, mob);
            send_packet(proto::play_cb::kSpawnMob, spawn.data());
            if (mob.type == 51) {
                ByteWriter equipment;
                encode_skeleton_bow(equipment, mob.entity_id);
                send_packet(proto::play_cb::kEntityEquipment, equipment.data());
            }
        }
    }
    if (context_.item_drops != nullptr) {
        for (const auto& drop : context_.item_drops->snapshot()) {
            if (!in_chunk(drop.x, drop.z, cx, cz)) {
                continue;
            }
            ByteWriter spawn;
            ByteWriter meta;
            encode_dropped_item(drop, spawn, meta);
            send_packet(proto::play_cb::kSpawnObject, spawn.data());
            send_packet(proto::play_cb::kEntityMetadata, meta.data());
        }
    }
}

void Connection::send_existing_mobs() {
    if (context_.mobs == nullptr) {
        return;
    }
    for (const auto& mob : context_.mobs->snapshot()) {
        ByteWriter spawn;
        encode_spawn_mob(spawn, mob);
        send_packet(proto::play_cb::kSpawnMob, spawn.data());
        if (mob.type == 51) {
            ByteWriter equipment;
            encode_skeleton_bow(equipment, mob.entity_id);
            send_packet(proto::play_cb::kEntityEquipment, equipment.data());
        }
    }
}

}  // namespace cyane::net
