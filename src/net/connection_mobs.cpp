#include "cyane/net/connection.hpp"

#include <algorithm>

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
    writers::write_spawn_mob(spawn, mob->entity_id, mob->type, mob->pos.x, mob->pos.y, mob->pos.z,
                             mob->pos.yaw);
    send_packet(proto::play_cb::kSpawnMob, spawn.data());
    if (context_.hub != nullptr) {
        if (const auto cpos = world::ChunkPos::from_world(static_cast<std::int32_t>(x),
                                                           static_cast<std::int32_t>(z))) {
            const std::int32_t radius = std::clamp(context_.view_distance, 2, 8);
            context_.hub->broadcast_near(cpos->x, cpos->z, radius, player_id_,
                                         proto::play_cb::kSpawnMob, spawn.data());
        }
    }
    return true;
}

void Connection::send_existing_mobs() {
    if (context_.mobs == nullptr) {
        return;
    }
    for (const auto& mob : context_.mobs->snapshot()) {
        ByteWriter spawn;
        writers::write_spawn_mob(spawn, mob.entity_id, mob.type, mob.pos.x, mob.pos.y, mob.pos.z,
                                 mob.pos.yaw);
        send_packet(proto::play_cb::kSpawnMob, spawn.data());
    }
}

}  // namespace cyane::net
