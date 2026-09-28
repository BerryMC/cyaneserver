#include "cyane/net/connection.hpp"

#include "cyane/net/mob_manager.hpp"

namespace cyane::net {

namespace {
// SpawnMob (0x03) 与移动广播共用：varint id | uuid(16) | varint type | x/y/z f64
// | yaw i8 | pitch i8 | headPitch i8 | velocity 3×i16 | metadata(0xFF 空)
void encode_spawn_mob(ByteWriter& out, const Mob& mob) {
    out.varint(static_cast<std::int32_t>(mob.entity_id));
    std::array<std::uint8_t, 16> uuid{};
    uuid[15] = static_cast<std::uint8_t>(mob.entity_id & 0xFF);
    uuid[14] = static_cast<std::uint8_t>((mob.entity_id >> 8) & 0xFF);
    out.bytes(ByteSpan{reinterpret_cast<const std::byte*>(uuid.data()), uuid.size()});
    out.varint(mob.type);
    out.f64(mob.pos.x);
    out.f64(mob.pos.y);
    out.f64(mob.pos.z);
    out.u8(angle_byte(mob.pos.yaw));
    out.u8(0);  // pitch
    out.u8(angle_byte(mob.pos.yaw));  // headPitch
    out.i16(0);
    out.i16(0);
    out.i16(0);
    out.u8(0xFF);  // 空 metadata 终止符
}
}  // namespace

void Connection::send_existing_mobs() {
    if (context_.mobs == nullptr) {
        return;
    }
    for (const auto& mob : context_.mobs->snapshot()) {
        ByteWriter spawn;
        encode_spawn_mob(spawn, mob);
        send_packet(proto::play_cb::kSpawnMob, spawn.data());
    }
}

}  // namespace cyane::net
