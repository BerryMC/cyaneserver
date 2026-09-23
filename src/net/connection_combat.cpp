#include "cyane/net/connection.hpp"

#include "cyane/core/log.hpp"

namespace cyane::net {

bool Connection::handle_play_client_command(ByteSpan payload) {
    // ClientCommand (0x03)：varint actionId。0=请求重生，1=请求统计
    ByteReader reader{payload};
    auto action = reader.varint();
    if (!action) {
        return false;
    }
    if (*action == 0 && dead_) {
        respawn_player();
    }
    return true;
}

void Connection::kill_player() {
    if (dead_) {
        return;
    }
    dead_ = true;
    health_ = 0.0f;
    // UpdateHealth (0x41)：float health=0 | varint food | float saturation
    // 血量为 0 时客户端显示死亡界面，等待玩家点“重生”回 ClientCommand(0)
    cyane::ByteWriter health;
    health.f32(0.0f);
    health.varint(20);
    health.f32(0.0f);
    send_packet(proto::play_cb::kUpdateHealth, health.data());
    log::info("{} died", username_);
}

void Connection::respawn_player() {
    dead_ = false;
    health_ = 20.0f;
    player_pos_ = spawn_point();

    // Respawn (0x35)：int dimension | byte difficulty | byte gameMode | string levelType
    // 客户端收到后卸载当前世界、清空区块缓存并等待新的地形
    cyane::ByteWriter respawn;
    respawn.i32(0);  // overworld
    respawn.u8(2);   // normal
    respawn.u8(context_.game_mode);
    respawn.string("default");
    send_packet(proto::play_cb::kRespawn, respawn.data());

    // 血量恢复
    cyane::ByteWriter health;
    health.f32(20.0f);
    health.varint(20);
    health.f32(5.0f);
    send_packet(proto::play_cb::kUpdateHealth, health.data());

    // 重新下发出生点区块并把玩家放回去
    loaded_chunks_.clear();
    has_center_ = false;
    const auto spawn_chunk = world::ChunkPos::from_world(
        static_cast<std::int32_t>(player_pos_.x), static_cast<std::int32_t>(player_pos_.z));
    update_view(spawn_chunk.value_or(world::ChunkPos{0, 0}));

    ++teleport_id_;
    cyane::ByteWriter tp;
    tp.f64(player_pos_.x);
    tp.f64(player_pos_.y);
    tp.f64(player_pos_.z);
    tp.f32(player_pos_.yaw);
    tp.f32(player_pos_.pitch);
    tp.u8(0);
    tp.varint(teleport_id_);
    send_packet(proto::play_cb::kPlayerPositionLook, tp.data());

    // 向他人广播复活后的位置
    if (context_.hub != nullptr) {
        context_.hub->update_position(player_id_, player_pos_.x, player_pos_.y, player_pos_.z,
                                      player_pos_.yaw, player_pos_.pitch);
    }
    log::info("{} respawned", username_);
}

}
