#include "cyane/net/connection.hpp"

#include <algorithm>
#include <array>

#include "cyane/item/item_stack.hpp"
#include "cyane/net/item_drop.hpp"
#include "cyane/net/mob_manager.hpp"
#include "cyane/net/packet_writers.hpp"

namespace cyane::net {
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
    writers::encode_spawn_mob(spawn, mob->entity_id, mob->type, mob->pos.x, mob->pos.y, mob->pos.z,
                             mob->pos.yaw);
    send_packet(proto::play_cb::kSpawnMob, spawn.data());
    ByteWriter equipment;
    if (mob->type == 51) {
        writers::encode_skeleton_bow(equipment, mob->entity_id);
        send_packet(proto::play_cb::kEntityEquipment, equipment.data());
    }
    if (context_.hub != nullptr) {
        if (const auto cpos = world::ChunkPos::from_world(x, z)) {
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
            if (mob.death_timer >= 0) {
                // 死亡动画中的生物：补发会以活体形象出现（客户端没收到它的
                // EntityStatus 3），动画结束随即被销毁——直接不发
                continue;
            }
            ByteWriter spawn;
            writers::encode_spawn_mob(spawn, mob.entity_id, mob.type, mob.pos.x, mob.pos.y,
                                      mob.pos.z, mob.pos.yaw);
            send_packet(proto::play_cb::kSpawnMob, spawn.data());
            if (mob.type == 51) {
                ByteWriter equipment;
                writers::encode_skeleton_bow(equipment, mob.entity_id);
                send_packet(proto::play_cb::kEntityEquipment, equipment.data());
            }
        }
    }
    if (context_.item_drops != nullptr) {
        for (const auto& drop : context_.item_drops->snapshot()) {
            if (drop.stack.empty() || !in_chunk(drop.x, drop.z, cx, cz)) {
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

}  // namespace cyane::net
