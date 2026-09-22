#include "cyane/net/connection.hpp"

#include <format>

#include "cyane/core/log.hpp"
#include "cyane/proto/frame.hpp"

namespace cyane::net {

Connection::Connection(Socket socket, std::string peer, Reactor& reactor, ConnectionContext context)
    : socket_(std::move(socket)), peer_(std::move(peer)), reactor_(&reactor),
      context_(std::move(context)), inbox_(kMaxInboxBytes), scratch_() {
    log::debug("connection {} -> {}", fd(), peer_);
}

Connection::~Connection() {
    teardown();
}

void Connection::on_readable() {
    while (true) {
        IoResult io = socket_.try_read(MutableByteSpan{inbox_}.subspan(inbox_offset_});
        if (io.status == IoStatus::closed) {
            teardown();
            return;
        }
        if (io.status == IoStatus::error) {
            log::warn("connection {} read error", fd());
            teardown();
            return;
        }
        if (io.status == IoStatus::would_block) {
            break;
        }
        inbox_offset_ += io.bytes;
        process_inbox();
    }
}

void Connection::on_writable() {
    flush_outbox();
}

void Connection::on_error() {
    log::warn("connection {} error", fd());
    teardown();
}

void Connection::teardown() noexcept {
    if (!alive_) {
        return;
    }
    alive_ = false;
    reactor_->remove(fd());
    disconnect("server error");
}

void Connection::process_inbox() {
    while (inbox_offset_ >= sizeof(std::int32_t)) {
        ByteReader reader{inbox_};
        reader.skip(inbox_offset_);
        auto maybe_packet_id = reader.varint();
        if (!maybe_packet_id) {
            log::warn("connection {} invalid packet id", fd());
            teardown();
            return;
        }
        const std::int32_t packet_id = *maybe_packet_id;
        if (handle_packet(packet_id, reader.rest())) {
            inbox_offset_ -= static_cast<std::size_t>(reader.varint() - packet_id);
        } else {
            teardown();
            return;
        }
    }
    if (inbox_offset_ > 0) {
        std::memmove(inbox_.data(), inbox_.data() + inbox_offset_, inbox_offset_);
    }
    inbox_offset_ = 0;
}

bool Connection::handle_packet(std::int32_t packet_id, ByteSpan payload) {
    if (state_ == proto::State::handshake) {
        return handle_handshake(payload);
    }
    if (state_ == proto::State::status) {
        return handle_status(packet_id, payload);
    }
    if (state_ == proto::State::login) {
        return handle_login(packet_id, payload);
    }
    if (state_ == proto::State::play) {
        return handle_play(packet_id, payload);
    }
    log::warn("connection {} unknown state", fd());
    return false;
}

bool Connection::handle_handshake(ByteSpan payload) {
    ByteReader reader{payload};
    auto maybe_protocol = reader.varint();
    if (!maybe_protocol) {
        return false;
    }
    protocol_version_ = *maybe_protocol;
    // 暂不支持主机名和端口解析
    auto maybe_next_state = reader.varint();
    if (!maybe_next_state) {
        return false;
    }
    // 根据next_state切换状态机
    state_ = static_cast<proto::State>(*maybe_next_state);
    return true;
}

bool Connection::handle_status(std::int32_t packet_id, ByteSpan payload) {
    if (packet_id == proto::status_sb::kRequest) {
        // 发送状态响应JSON
        std::string json = context_.status ? context_.status->build_status_json() : "{}";
        cyane::ByteWriter response;
        response.string(std::move(json));
        send_packet(proto::status_cb::kResponse, response.data());
        return true;
    }
    if (packet_id == proto::status_sb::kPing) {
        ByteReader reader{payload};
        auto maybe_value = reader.varint();
        if (!maybe_value) {
            return false;
        }
        cyane::ByteWriter response;
        response.varint(*maybe_value);
        send_packet(proto::status_cb::kPong, response.data());
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
    if (context_.online_mode) {
        send_encryption_request();
        return true;
    }
    finish_login(context_.player_manager ? context_.player_manager->generate_uuid(username_) : "");
    return true;
}

bool Connection::handle_encryption_response(ByteSpan payload) {
    // 验证加密响应并完成登录
    // 简化实现：假设验证成功
    // 实际应用需要解密并验证verify_token等
    std::string uuid = context_.player_manager ? context_.player_manager->generate_uuid(username_) : "";
    finish_login(uuid);
    return true;
}

bool Connection::handle_play(std::int32_t packet_id, ByteSpan payload) {
    if (packet_id == proto::play_sb::kKeepAlive) {
        return handle_play_keepalive();
    }
    if (packet_id == proto::play_sb::kPosition || packet_id == proto::play_sb::kPositionLook) {
        return handle_play_position(packet_id, payload);
    }
    if (packet_id == proto::play_sb::kChatMessage) {
        return handle_play_chat(payload);
    }
    if (packet_id == proto::play_sb::kChunkRequest) {
        return handle_play_chunk_request(payload);
    }
    log::debug("connection {} unhandled play packet id {}", fd(), packet_id);
    return true;
}

bool Connection::handle_play_keepalive() {
    // 发送keepalive响应
    cyane::ByteWriter fields;
    fields.varint(keepalive_id_++);
    send_packet(proto::play_cb::kKeepAlive, fields.data());
    return true;
}

bool Connection::handle_play_position(std::int32_t packet_id, ByteSpan payload) {
    // 解析玩家位置
    if (payload.size() < 4 * sizeof(std::int64_t)) {
        return false;
    }
    ByteReader reader{payload};
    auto maybe_x = reader.i64();
    auto maybe_y = reader.i64();
    auto maybe_z = reader.i64();
    auto maybe_yaw = reader.i64();
    if (!maybe_x || !maybe_y || !maybe_z || !maybe_yaw) {
        return false;
    }
    Position pos;
    pos.x = static_cast<double>(*maybe_x) / 32.0;
    pos.y = static_cast<double>(*maybe_y) / 64.0;
    pos.z = static_cast<double>(*maybe_z) / 32.0;
    pos.yaw = static_cast<float>(*maybe_yaw) / 256.0f;
    update_player_position(pos);
    return true;
}

bool Connection::handle_play_chat(ByteSpan payload) {
    ByteReader reader{payload};
    auto maybe_message = reader.string();
    if (!maybe_message) {
        return false;
    }
    // 记录聊天消息
    log::info("connection {} chat: {}", fd(), *maybe_message);
    return true;
}

bool Connection::handle_play_chunk_request(ByteSpan payload) {
    // 解析区块请求
    if (payload.size() < sizeof(std::int32_t)) {
        return false;
    }
    ByteReader reader{payload};
    auto maybe_chunk_x = reader.i32();
    auto maybe_chunk_z = reader.i32();
    if (!maybe_chunk_x || !maybe_chunk_z) {
        return false;
    }
    // 为玩家生成Chunk Data包
    // 简化实现：发送一个空的Chunk Data
    cyane::ByteWriter chunk_fields;
    chunk_fields.varint(0);  // chunk X
    chunk_fields.varint(0);  // chunk Z
    chunk_fields.varint(0);  // 半砖数量
    chunk_fields.varint(0);  // 数据长度
    send_packet(proto::play_cb::kChunkData, chunk_fields.data());
    return true;
}

void Connection::update_player_position(entity::Position pos) {
    player_pos_ = pos;
    // 发送玩家信息包
    cyane::ByteWriter info_fields;
    info_fields.varint(proto::play_cb::kPlayerInfoAddPlayer);
    info_fields.varint(1);
    std::string uuid = context_.player_manager ? context_.player_manager->generate_uuid(username_) : "";
    info_fields.string(uuid);
    info_fields.string(username_);
    info_fields.varint(0);
    info_fields.varint(0);
    info_fields.varint(20);
    info_fields.boolean(false);
    send_packet(proto::play_cb::kPlayerInfo, info_fields.data());
    // 发送位置同步包
    cyane::ByteWriter pos_fields;
    pos_fields.i64(static_cast<std::int64_t>(player_pos_.x * 32.0));
    pos_fields.i64(static_cast<std::int64_t>(player_pos_.y * 64.0));
    pos_fields.i64(static_cast<std::int64_t>(player_pos_.z * 32.0));
    pos_fields.i64(static_cast<std::int64_t>(player_pos_.yaw * 256.0f));
    pos_fields.i32(0);  // 踩在哪个方块上
    send_packet(proto::play_cb::kPlayerPositionLook, pos_fields.data());
}

void Connection::send_spawn_player() {
    ByteWriter info_fields;
    info_fields.varint(proto::play_cb::kPlayerInfoAddPlayer);
    info_fields.varint(1);
    info_fields.string(uuid_with_dashes);
    info_fields.string(username_);
    info_fields.varint(0);
    info_fields.varint(0);
    info_fields.varint(20);
    info_fields.boolean(false);
    send_packet(proto::play_cb::kPlayerInfo, info_fields.data());
}

void Connection::send_encryption_request() {
    // 发送加密请求包
    cyane::ByteWriter fields;
    fields.string("")  // serverId
    fields.varint(context_.keys ? context_.keys->public_der().size() : 0);
    if (context_.keys) {
        fields.bytes(context_.keys->public_der());
    }
    fields.varint(4);  // verifyToken长度
    // 生成随机verify_token
    verify_token_.resize(4);
    if (!crypto::random_bytes(verify_token_)) {
        teardown();
        return;
    }
    fields.bytes(verify_token_);
    send_packet(proto::login_cb::kEncryptionRequest, fields.data());
}

void Connection::finish_login(std::string uuid_with_dashes) {
    // 发送压缩设置包
    cyane::ByteWriter compression_fields;
    compression_fields.varint(context_.compression_threshold);
    send_packet(proto::login_cb::kSetCompression, compression_fields.data());
    // 发送登录成功包
    cyane::ByteWriter success_fields;
    success_fields.string(std::move(uuid_with_dashes));
    success_fields.string(username_);
    send_packet(proto::login_cb::kSuccess, success_fields.data());
    // 切换到play状态
    state_ = proto::State::play;
    // 发送加入游戏包
    cyane::ByteWriter join_game_fields;
    join_game_fields.varint(2);  // 游戏模式
    join_game_fields.varint(0);  // 难度
    join_game_fields.varint(0);  // 游戏类型
    join_game_fields.varint(player_id_++);
    join_game_fields.string("default");  // world name
    join_game_fields.string("")  // 签名
    send_packet(proto::play_cb::kJoinGame, join_game_fields.data());
    // 发送出生点
    cyane::ByteWriter spawn_pos_fields;
    spawn_pos_fields.i64(static_cast<std::int64_t>(player_pos_.x * 32.0));
    spawn_pos_fields.i64(static_cast<std::int64_t>(player_pos_.y * 64.0));
    spawn_pos_fields.i64(static_cast<std::int64_t>(player_pos_.z * 32.0));
    spawn_pos_fields.i32(0);
    send_packet(proto::play_cb::kSpawnPosition, spawn_pos_fields.data());
    // 发送玩家信息
    send_spawn_player();
    // 发送生命值更新
    cyane::ByteWriter health_fields;
    health_fields.i32(20);  // 最大生命值
    health_fields.i32(20);  // 当前生命值
    health_fields.i32(0);   // 食物条
    health_fields.i32(0);   // 饥饿度
    health_fields.i32(0);   // 经验值
    send_packet(proto::play_cb::kUpdateHealth, health_fields.data());
    // 发送时间更新
    cyane::ByteWriter time_fields;
    time_fields.varint(0);  // 游戏时间
    send_packet(proto::play_cb::kTimeUpdate, time_fields.data());
}

void Connection::enable_cipher(ByteSpan session_key) {
    decrypt_cipher_ = std::make_unique<crypto::StreamCipher>(session_key, true);
    encrypt_cipher_ = std::make_unique<crypto::StreamCipher>(session_key, false);
}

void Connection::flush_outbox() {
    if (!want_write_ || outbox_offset_ == 0) {
        return;
    }
    IoResult io = socket_.try_write(ByteSpan{outbox_}.subspan(outbox_offset_});
    if (io.status == IoStatus::error) {
        log::warn("connection {} write error", fd());
        teardown();
        return;
    }
    if (io.status == IoStatus::would_block) {
        return;
    }
    outbox_offset_ -= io.bytes;
    if (outbox_offset_ > 0) {
        std::memmove(outbox_.data(), outbox_.data() + io.bytes, outbox_offset_);
    }
    if (outbox_offset_ == 0) {
        want_write_ = false;
        reactor_->set_writable(fd(), *this, false);
    }
}

void Connection::set_writable(bool writable) {
    if (writable != want_write_) {
        want_write_ = writable;
        reactor_->set_writable(fd(), *this, writable);
    }
}

void Connection::send_packet(std::int32_t packet_id, ByteSpan fields) {
    if (!alive_) {
        return;
    }
    // 压缩后的帧长度 = 长度前缀 + 数据
    Bytes frame;
    frame.reserve(fields.size() + varint_size(static_cast<std::int32_t>(fields.size())) + varint_size(packet_id));
    proto::encode_frame(frame, packet_id, fields, context_.compression_threshold);
    if (frame.size() > kOutboxHighWater) {
        log::warn("connection {} outbox too large", fd());
        teardown();
        return;
    }
    if (outbox_offset_ + frame.size() > outbox_.size()) {
        // outbox 容量不足，扩展
        outbox_.resize(std::max(outbox_.size() * 2, outbox_offset_ + frame.size()));
    }
    std::memcpy(outbox_.data() + outbox_offset_, frame.data(), frame.size());
    outbox_offset_ += frame.size();
    set_writable(true);
}

void Connection::disconnect(std::string_view reason) {
    if (!alive_) {
        return;
    }
    alive_ = false;
    // 发送断开包
    cyane::ByteWriter fields;
    fields.string(std::string{reason});
    send_packet(proto::play_cb::kDisconnect, fields.data());
    close_after_flush_ = true;
}

}