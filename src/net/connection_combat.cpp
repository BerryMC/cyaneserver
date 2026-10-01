#include "cyane/net/connection.hpp"

#include "cyane/core/log.hpp"
#include "cyane/net/packet_writers.hpp"

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
    // EntityStatus 3 = 死亡动画，向他人广播
    if (context_.hub != nullptr) {
        cyane::ByteWriter status;
        writers::write_entity_status(status, player_id_, 3);
        context_.hub->broadcast(player_id_, proto::play_cb::kEntityStatus, status.data());
    }
    log::info("{} died", username_);
}

void Connection::respawn_player() {
    dead_ = false;
    health_ = 20.0f;
    player_pos_ = spawn_point();

    // Respawn (0x35)：int dimension | byte difficulty | byte gameMode | string levelType
    // 1.12.2 客户端同维度重生不重置世界；对齐 vanilla：不重发 JoinGame
    // （实测 Respawn 后再发 JoinGame 会强制客户端重建世界并卡在"加载地形"）
    cyane::ByteWriter respawn;
    respawn.i32(0);  // overworld
    respawn.u8(2);   // normal
    respawn.u8(context_.game_mode);
    respawn.string("default");
    send_packet(proto::play_cb::kRespawn, respawn.data());

    // vanilla 重生序列：Abilities → Health → SpawnPosition → TimeUpdate → HeldItem
    send_abilities_for(context_.game_mode);
    cyane::ByteWriter health;
    health.f32(20.0f);
    health.varint(20);
    health.f32(5.0f);
    send_packet(proto::play_cb::kUpdateHealth, health.data());
    cyane::ByteWriter spawn_pos;
    spawn_pos.position(context_.spawn_x, context_.spawn_y, context_.spawn_z);
    send_packet(proto::play_cb::kSpawnPosition, spawn_pos.data());
    cyane::ByteWriter time;
    time.i64(0);
    time.i64(0);
    send_packet(proto::play_cb::kTimeUpdate, time.data());
    cyane::ByteWriter held;
    writers::write_held_item_change(held, selected_slot_);
    send_packet(proto::play_cb::kHeldItemChange, held.data());

    // 出生点区块可能已被客户端按 UnloadChunk 丢弃：清表重发（重复 ChunkData 就地覆盖）
    loaded_chunks_.clear();
    pending_chunks_.clear();
    pending_chunk_keys_.clear();
    has_center_ = false;
    const auto spawn_chunk = world::ChunkPos::from_world(
        static_cast<std::int32_t>(player_pos_.x), static_cast<std::int32_t>(player_pos_.z));
    update_view(spawn_chunk.value_or(world::ChunkPos{0, 0}));

    // 同维度重生客户端不清世界：不补发实体（避免双份）；
    // 他人端销毁旧实体后按新位置重发本玩家的 SpawnPlayer
    broadcast_despawn();
    broadcast_spawn();

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
