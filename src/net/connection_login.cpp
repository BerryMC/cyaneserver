#include "cyane/net/connection.hpp"

#include <format>

#include "cyane/core/log.hpp"
#include "cyane/crypto/digest.hpp"
#include "cyane/proto/json.hpp"

namespace cyane::net {

bool Connection::handle_handshake(ByteSpan payload) {
    // protocol VarInt | host String | port u16 | next VarInt
    ByteReader reader{payload};
    auto maybe_protocol = reader.varint();
    if (!maybe_protocol) {
        return false;
    }
    protocol_version_ = *maybe_protocol;

    auto maybe_host = reader.string(255);
    if (!maybe_host) {
        return false;
    }
    auto maybe_port = reader.u16();
    if (!maybe_port) {
        return false;
    }

    auto maybe_next_state = reader.varint();
    if (!maybe_next_state) {
        return false;
    }
    const std::int32_t next_state = *maybe_next_state;
    if (next_state != static_cast<std::int32_t>(proto::State::status) &&
        next_state != static_cast<std::int32_t>(proto::State::login)) {
        log::warn("connection {} bad next state {}", fd(), next_state);
        return false;
    }
    state_ = static_cast<proto::State>(next_state);
    return true;
}

bool Connection::handle_status(std::int32_t packet_id, ByteSpan payload) {
    if (packet_id == proto::status_sb::kRequest) {
        std::string json = context_.status ? context_.status->build_status_json() : "{}";
        cyane::ByteWriter response;
        response.string(std::move(json));
        send_packet(proto::status_cb::kResponse, response.data());
        return true;
    }
    if (packet_id == proto::status_sb::kPing) {
        // Pong：原样回显 Ping 负载
        send_packet(proto::status_cb::kPong, payload);
        return true;
    }
    log::warn("connection {} unknown status packet id {}", fd(), packet_id);
    return false;
}

bool Connection::handle_login(std::int32_t packet_id, ByteSpan payload) {
    if (packet_id == proto::login_sb::kLoginStart) {
        return handle_login_start(payload);
    }
    if (packet_id == proto::login_sb::kEncryptionResponse) {
        return handle_encryption_response(payload);
    }
    log::warn("connection {} unknown login packet id {}", fd(), packet_id);
    return false;
}

bool Connection::handle_login_start(ByteSpan payload) {
    ByteReader reader{payload};
    auto maybe_name = reader.string();
    if (!maybe_name) {
        return false;
    }
    username_ = std::move(*maybe_name);

    // 协议版本不匹配：回复断开原因（含 Outdated client/server）
    if (protocol_version_ != proto::kProtocolVersion) {
        const std::string reason = protocol_version_ < proto::kProtocolVersion
            ? std::format("Outdated client! Please use {}", proto::kMinecraftVersion)
            : std::format("Outdated server! I'm still on {}", proto::kMinecraftVersion);
        cyane::ByteWriter fields;
        fields.string(proto::chat_text(reason));
        send_packet(proto::login_cb::kDisconnect, fields.data());
        close_after_flush_ = true;
        return true;
    }

    if (context_.online_mode) {
        send_encryption_request();
        return true;
    }
    finish_login(crypto::offline_uuid(username_));
    return true;
}

bool Connection::handle_encryption_response(ByteSpan payload) {
    (void)payload;
    // TODO(online-mode): 解密并校验 verify_token，当前按 offline 直接放行
    finish_login(crypto::offline_uuid(username_));
    return true;
}

void Connection::send_encryption_request() {
    cyane::ByteWriter fields;
    fields.string("");  // serverId
    fields.varint(static_cast<std::int32_t>(context_.keys ? context_.keys->public_der().size() : 0));
    if (context_.keys) {
        fields.bytes(context_.keys->public_der());
    }
    fields.varint(4);  // verifyToken 长度
    const auto verify_token = crypto::random_bytes(4);
    if (verify_token.empty()) {
        teardown();
        return;
    }
    fields.bytes(verify_token);
    send_packet(proto::login_cb::kEncryptionRequest, fields.data());
}

void Connection::finish_login(Uuid uuid) {
    if (context_.compression_threshold >= 0) {
        cyane::ByteWriter compression_fields;
        compression_fields.varint(context_.compression_threshold);
        send_packet(proto::login_cb::kSetCompression, compression_fields.data());
        compression_threshold_ = context_.compression_threshold;
    }
    uuid_ = uuid;
    send_login_success(uuid.dashed());

    state_ = proto::State::play;
    player_id_ = entity::allocate_entity_id();
    player_pos_ = spawn_point();
    load_player_data();

    send_join_game();
    send_world_state();
    send_initial_teleport();
    register_in_hub();
}

entity::Position Connection::spawn_point() const noexcept {
    // 以世界出生点（level.dat）为网格原点，按实体 id 错开：
    // 多名玩家重叠在同一坐标时，客户端视锥剔除会在某些视角把对方剔除。
    const double x = static_cast<double>(context_.spawn_x) + 0.5 +
                     static_cast<double>(player_id_ % 8) * 2.0;
    const double z = static_cast<double>(context_.spawn_z) + 0.5 +
                     static_cast<double>((player_id_ / 8) % 8) * 2.0;
    // level.dat 的 SpawnY 可能过时（世界重建/地形变化）——落到实际地表
    const double y = context_.world != nullptr
                         ? static_cast<double>(
                               context_.world->surface_y(context_.spawn_x, context_.spawn_z))
                         : static_cast<double>(context_.spawn_y);
    return entity::Position{x, y, z, 0.0f, 0.0f};
}

void Connection::send_login_success(std::string uuid_with_dashes) {
    cyane::ByteWriter fields;
    fields.string(std::move(uuid_with_dashes));
    fields.string(username_);
    send_packet(proto::login_cb::kSuccess, fields.data());
    log::info("{} joined the game as {}", peer_, username_);
}

void Connection::send_join_game() {
    // JoinGame (0x23)：int entityId | byte gameMode | int dimension | byte difficulty
    //                 | byte maxPlayers | string levelType | bool reducedDebug
    cyane::ByteWriter fields;
    fields.i32(static_cast<std::int32_t>(player_id_));
    fields.u8(context_.game_mode);
    fields.i32(0);  // overworld
    fields.u8(2);   // normal
    fields.u8(static_cast<std::uint8_t>(std::min<std::int32_t>(context_.max_players, 255)));
    fields.string("default");
    fields.boolean(false);
    send_packet(proto::play_cb::kJoinGame, fields.data());

    // 所有模式都发 PlayerAbilities (0x2C)：创造/旁观开飞行，生存/冒险显式清零
    // creativeMode 位，客户端据此切换物品栏界面与血条/饥饿条显示
    send_abilities_for(context_.game_mode);
}

void Connection::send_world_state() {
    cyane::ByteWriter spawn_pos;
    spawn_pos.position(context_.spawn_x, context_.spawn_y, context_.spawn_z);
    send_packet(proto::play_cb::kSpawnPosition, spawn_pos.data());

    if (context_.player_manager != nullptr) {
        context_.player_manager->add(player_id_, username_, player_pos_);
    }
    send_spawn_player();

    cyane::ByteWriter health;
    health.f32(20.0f);
    health.varint(20);
    health.f32(5.0f);
    send_packet(proto::play_cb::kUpdateHealth, health.data());

    cyane::ByteWriter time;
    time.i64(0);
    time.i64(0);
    send_packet(proto::play_cb::kTimeUpdate, time.data());

    const auto spawn_chunk = world::ChunkPos::from_world(player_pos_.x, player_pos_.z);
    update_view(spawn_chunk.value_or(world::ChunkPos{0, 0}));
    send_pending_chunks(49);

    // 同步整份背包（windowId=0）：客户端据此渲染物品栏
    send_inventory();
    // 补发世界中已有的掉落物实体
}

void Connection::send_initial_teleport() {
    // PlayerPositionLook (0x2F)：把玩家放到出生点，客户端须回 ConfirmTeleport
    teleport_id_ = 1;
    cyane::ByteWriter fields;
    fields.f64(player_pos_.x);
    fields.f64(player_pos_.y);
    fields.f64(player_pos_.z);
    fields.f32(player_pos_.yaw);
    fields.f32(player_pos_.pitch);
    fields.u8(0);  // flags：绝对坐标
    fields.varint(teleport_id_);
    send_packet(proto::play_cb::kPlayerPositionLook, fields.data());
}

}
