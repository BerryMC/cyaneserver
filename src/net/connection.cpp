#include "cyane/net/connection.hpp"

#include <algorithm>
#include <cstdlib>
#include <format>
#include <vector>

#include "cyane/core/log.hpp"
#include "cyane/crypto/digest.hpp"
#include "cyane/proto/frame.hpp"
#include "cyane/proto/json.hpp"
#include "cyane/world/chunk_codec.hpp"

namespace cyane::net {

namespace {
// 角度 float(度) → 1/256 圈的字节角
[[nodiscard]] std::uint8_t to_angle_byte(float degrees) noexcept {
    return static_cast<std::uint8_t>(static_cast<int>(degrees * 256.0f / 360.0f) & 0xFF);
}

// 区块坐标打包成 64 位键，用于集合去重
[[nodiscard]] std::int64_t chunk_key(world::ChunkPos pos) noexcept {
    return (static_cast<std::int64_t>(pos.x) << 32) | (static_cast<std::uint32_t>(pos.z));
}

void write_player_info_add(ByteWriter& out, const std::array<std::uint8_t, 16>& uuid,
                           std::string_view name) {
    out.varint(proto::play_cb::kPlayerInfoAddPlayer);
    out.varint(1);
    out.bytes(ByteSpan{reinterpret_cast<const std::byte*>(uuid.data()), uuid.size()});
    out.string(name);
    out.varint(0);   // properties
    out.varint(0);   // gameMode
    out.varint(20);  // ping
    out.boolean(false);
}

// NamedEntitySpawn (0x05)：varint id | uuid(16) | double x/y/z | byte yaw | byte pitch | metadata(0xff 终止)
void write_named_spawn(ByteWriter& out, std::uint32_t entity_id,
                       const std::array<std::uint8_t, 16>& uuid, double x, double y, double z,
                       float yaw, float pitch) {
    out.varint(static_cast<std::int32_t>(entity_id));
    out.bytes(ByteSpan{reinterpret_cast<const std::byte*>(uuid.data()), uuid.size()});
    out.f64(x);
    out.f64(y);
    out.f64(z);
    out.u8(to_angle_byte(yaw));
    out.u8(to_angle_byte(pitch));
    out.u8(0xFF);
}
}

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
        IoResult io = socket_.try_read(MutableByteSpan{inbox_}.subspan(inbox_offset_));
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
    // 处理完入站数据后立即尝试冲刷出站队列，避免依赖 EPOLLOUT 边沿时序
    if (alive_ && outbox_offset_ > 0) {
        want_write_ = true;
        flush_outbox();
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
    broadcast_despawn();
    if (context_.player_manager != nullptr && player_id_ != 0) {
        context_.player_manager->remove(player_id_);
    }
    if (reactor_ != nullptr) {
        reactor_->remove(fd());
    }
    socket_.close();
}

void Connection::process_inbox() {
    // inbox_offset_ 表示已缓冲的有效字节数；cursor 是本轮解析的读游标
    std::size_t cursor = 0;
    while (cursor < inbox_offset_) {
        ByteReader reader{ByteSpan{inbox_.data() + cursor, inbox_offset_ - cursor}};
        auto maybe_frame_size = reader.varint();
        if (!maybe_frame_size) {
            // 帧长度前缀不完整，等待更多数据
            break;
        }
        const std::int32_t frame_size = *maybe_frame_size;
        if (frame_size < 0 || frame_size > proto::kMaxFrameBytes) {
            log::warn("connection {} bad frame length {}", fd(), frame_size);
            teardown();
            return;
        }

        const std::size_t header_size = reader.offset();
        const std::size_t total_size = header_size + static_cast<std::size_t>(frame_size);
        if (cursor + total_size > inbox_offset_) {
            // 帧不完整，等待更多数据
            break;
        }

        ByteSpan frame_body{inbox_.data() + cursor + header_size, static_cast<std::size_t>(frame_size)};
        auto decoded = proto::decode_frame(frame_body, scratch_, compression_threshold_);
        if (!decoded) {
            log::warn("connection {} invalid frame: {}", fd(), decoded.error().message);
            teardown();
            return;
        }
        if (!handle_packet(decoded->packet_id, decoded->payload)) {
            teardown();
            return;
        }
        cursor += total_size;
    }
    // 将未消费的尾部数据挪到缓冲区开头
    if (cursor > 0) {
        const std::size_t leftover = inbox_offset_ - cursor;
        if (leftover > 0) {
            std::memmove(inbox_.data(), inbox_.data() + cursor, leftover);
        }
        inbox_offset_ = leftover;
    }
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

bool Connection::handle_play(std::int32_t packet_id, ByteSpan payload) {
    if (packet_id == proto::play_sb::kConfirmTeleport) {
        return true;
    }
    if (packet_id == proto::play_sb::kKeepAlive) {
        return handle_play_keepalive(payload);
    }
    // 移动：Position(0x0D) / PositionLook(0x0E) / Look(0x0F) / Flying(0x0C)
    if (packet_id == proto::play_sb::kPosition || packet_id == proto::play_sb::kPositionLook ||
        packet_id == proto::play_sb::kLook || packet_id == proto::play_sb::kFlying) {
        return handle_play_position(packet_id, payload);
    }
    if (packet_id == proto::play_sb::kEntityAction) {
        return handle_play_entity_action(payload);
    }
    if (packet_id == proto::play_sb::kChatMessage) {
        return handle_play_chat(payload);
    }
    // 已知但暂无游戏逻辑的 serverbound 包：静默接受，避免日志刷屏
    switch (packet_id) {
        case proto::play_sb::kSettings:                  // 客户端设置（视距/语言/皮肤部件）
        case proto::play_sb::kPluginMessage:             // 插件通道（MC|Brand 等）
        case proto::play_sb::kAbilities:                 // 飞行能力回报
        case proto::play_sb::kHeldItemChange:            // 切换手持栏位
        case proto::play_sb::kAnimation:                 // 挥手动画
        case proto::play_sb::kClientCommand:             // 重生/统计请求
        case proto::play_sb::kCloseWindow:               // 关闭窗口
        case proto::play_sb::kRecipeDisplayed:           // 配方书
        case proto::play_sb::kUseItem:                   // 使用物品
        case proto::play_sb::kPlayerDigging:             // 挖掘（M3 再实现）
        case proto::play_sb::kBlockPlace:                // 放置（M3 再实现）
            return true;
        default:
            break;
    }
    log::debug("connection {} unhandled play packet id 0x{:02x}", fd(), packet_id);
    return true;
}

bool Connection::handle_play_keepalive(ByteSpan payload) {
    // long id 须与最近发出的心跳一致
    ByteReader reader{payload};
    auto id = reader.i64();
    if (!id) {
        return false;
    }
    if (awaiting_keepalive_ && *id == last_keepalive_id_) {
        awaiting_keepalive_ = false;
    }
    return true;
}

void Connection::tick(std::uint64_t now_ms) {
    if (!alive_ || state_ != proto::State::play) {
        return;
    }
    // 先投递他人广播来的消息（进入 play 后 hub_entry_ 有效）
    drain_mailbox();
    // 首次进入 play：以当前时间作为存活基线
    if (last_keepalive_recv_ms_ == 0) {
        last_keepalive_recv_ms_ = now_ms;
        last_keepalive_sent_ms_ = now_ms;
    }
    // 已收到回复：刷新存活基线
    if (!awaiting_keepalive_) {
        last_keepalive_recv_ms_ = now_ms;
    }
    // 超时未回：断开
    if (awaiting_keepalive_ && now_ms - last_keepalive_recv_ms_ > kKeepAliveTimeoutMs) {
        disconnect("Timed out");
        return;
    }
    // 到间隔且上一个已回：发新的 KeepAlive（KeepAlive 计时用 now 的低位做 id）
    if (!awaiting_keepalive_ && now_ms - last_keepalive_sent_ms_ >= kKeepAliveIntervalMs) {
        last_keepalive_id_ = static_cast<std::int64_t>(now_ms);
        last_keepalive_sent_ms_ = now_ms;
        awaiting_keepalive_ = true;
        cyane::ByteWriter fields;
        fields.i64(last_keepalive_id_);
        send_packet(proto::play_cb::kKeepAlive, fields.data());
    }
}

bool Connection::handle_play_position(std::int32_t packet_id, ByteSpan payload) {
    ByteReader reader{payload};
    entity::Position pos = player_pos_;
    if (packet_id == proto::play_sb::kPosition || packet_id == proto::play_sb::kPositionLook) {
        auto x = reader.f64();
        auto y = reader.f64();
        auto z = reader.f64();
        if (!x || !y || !z) {
            return false;
        }
        pos.x = *x;
        pos.y = *y;
        pos.z = *z;
    }
    if (packet_id == proto::play_sb::kPositionLook || packet_id == proto::play_sb::kLook) {
        auto yaw = reader.f32();
        auto pitch = reader.f32();
        if (!yaw || !pitch) {
            return false;
        }
        pos.yaw = *yaw;
        pos.pitch = *pitch;
    }
    player_pos_ = pos;
    if (context_.player_manager != nullptr) {
        context_.player_manager->update_position(player_id_, pos);
    }
    broadcast_movement(pos);
    const auto chunk = world::ChunkPos::from_world(
        static_cast<std::int32_t>(player_pos_.x), static_cast<std::int32_t>(player_pos_.z));
    if (chunk && (!has_center_ || *chunk != last_center_)) {
        update_view(*chunk);
    }
    return true;
}

void Connection::broadcast_movement(const entity::Position& pos) {
    if (context_.hub == nullptr) {
        return;
    }
    context_.hub->update_position(player_id_, pos.x, pos.y, pos.z, pos.yaw, pos.pitch);
    const std::uint8_t angle_yaw = to_angle_byte(pos.yaw);
    const std::uint8_t angle_pitch = to_angle_byte(pos.pitch);
    // EntityTeleport (0x4C)：绝对坐标最稳，避免相对移动累积误差
    ByteWriter tp;
    tp.varint(static_cast<std::int32_t>(player_id_));
    tp.f64(pos.x);
    tp.f64(pos.y);
    tp.f64(pos.z);
    tp.u8(angle_yaw);
    tp.u8(angle_pitch);
    tp.boolean(true);
    context_.hub->broadcast(player_id_, proto::play_cb::kEntityTeleport, tp.data());
    ByteWriter head;
    head.varint(static_cast<std::int32_t>(player_id_));
    head.u8(angle_yaw);
    context_.hub->broadcast(player_id_, proto::play_cb::kEntityHeadLook, head.data());
}

bool Connection::handle_play_entity_action(ByteSpan payload) {
    // EntityAction (0x15)：varint entityId | varint action | varint jumpBoost
    // action：0 开始潜行 1 停止潜行 3 开始疾跑 4 停止疾跑 …（EnumPlayerAction 序号）
    ByteReader reader{payload};
    auto entity_id = reader.varint();
    auto action = reader.varint();
    auto jump_boost = reader.varint();
    if (!entity_id || !action || !jump_boost) {
        return false;
    }
    switch (*action) {
        case 0: sneaking_ = true; break;
        case 1: sneaking_ = false; break;
        case 3: sprinting_ = true; break;
        case 4: sprinting_ = false; break;
        default: break;  // 睡眠/骑乘跳/开背包/滑翔：暂不处理
    }
    return true;
}

bool Connection::handle_play_chat(ByteSpan payload) {
    ByteReader reader{payload};
    auto maybe_message = reader.string(256);
    if (!maybe_message) {
        return false;
    }
    log::info("<{}> {}", username_, *maybe_message);

    // 组装 "<name> message" 聊天组件，广播给所有客户端（含自己）
    const std::string line = std::format("<{}> {}", username_, *maybe_message);
    ByteWriter chat;
    chat.string(proto::chat_text(line));
    chat.u8(0);  // position：0 = 聊天框
    if (context_.hub != nullptr) {
        context_.hub->broadcast_all(proto::play_cb::kChatMessage, chat.data());
    } else {
        send_packet(proto::play_cb::kChatMessage, chat.data());
    }
    return true;
}

void Connection::send_spawn_player() {
    ByteWriter info;
    write_player_info_add(info, uuid_bytes_, username_);
    send_packet(proto::play_cb::kPlayerInfo, info.data());
}

void Connection::broadcast_spawn() {
    if (context_.hub == nullptr) {
        return;
    }
    ByteWriter info;
    write_player_info_add(info, uuid_bytes_, username_);
    context_.hub->broadcast(player_id_, proto::play_cb::kPlayerInfo, info.data());

    ByteWriter spawn;
    write_named_spawn(spawn, player_id_, uuid_bytes_, player_pos_.x, player_pos_.y, player_pos_.z,
                      player_pos_.yaw, player_pos_.pitch);
    context_.hub->broadcast(player_id_, proto::play_cb::kSpawnPlayer, spawn.data());
}

void Connection::spawn_existing_players() {
    if (context_.hub == nullptr) {
        return;
    }
    for (const auto& other : context_.hub->others(player_id_)) {
        ByteWriter info;
        write_player_info_add(info, other.uuid, other.name);
        send_packet(proto::play_cb::kPlayerInfo, info.data());

        ByteWriter spawn;
        write_named_spawn(spawn, other.entity_id, other.uuid, other.x, other.y, other.z,
                          other.yaw, other.pitch);
        send_packet(proto::play_cb::kSpawnPlayer, spawn.data());
    }
}

void Connection::broadcast_despawn() {
    if (context_.hub == nullptr || player_id_ == 0) {
        return;
    }
    // DestroyEntities (0x32)：varint count | varint[] ids
    ByteWriter destroy;
    destroy.varint(1);
    destroy.varint(static_cast<std::int32_t>(player_id_));
    context_.hub->broadcast(player_id_, proto::play_cb::kDestroyEntities, destroy.data());
    // PlayerInfo(remove=4)：count | uuid(16)
    ByteWriter info;
    info.varint(proto::play_cb::kPlayerInfoRemovePlayer);
    info.varint(1);
    info.bytes(ByteSpan{reinterpret_cast<const std::byte*>(uuid_bytes_.data()), uuid_bytes_.size()});
    context_.hub->broadcast(player_id_, proto::play_cb::kPlayerInfo, info.data());

    context_.hub->unregister_player(player_id_);
    hub_entry_.reset();
}

void Connection::drain_mailbox() {
    if (!hub_entry_) {
        return;
    }
    std::vector<HubMessage> pending;
    {
        std::lock_guard<std::mutex> lock(hub_entry_->mailbox_mutex);
        pending.swap(hub_entry_->mailbox);
    }
    for (const auto& msg : pending) {
        send_packet(msg.packet_id, ByteSpan{msg.payload});
    }
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

void Connection::finish_login(std::string uuid_with_dashes) {
    if (context_.compression_threshold >= 0) {
        cyane::ByteWriter compression_fields;
        compression_fields.varint(context_.compression_threshold);
        send_packet(proto::login_cb::kSetCompression, compression_fields.data());
        compression_threshold_ = context_.compression_threshold;
    }
    // UUID 二进制须在广播前解析，供 PlayerInfo/SpawnPlayer 复用
    uuid_bytes_ = crypto::parse_uuid_string(uuid_with_dashes);
    send_login_success(std::move(uuid_with_dashes));

    state_ = proto::State::play;
    player_id_ = entity::allocate_entity_id();
    player_pos_ = spawn_point();

    send_join_game();
    send_world_state();
    send_initial_teleport();
    register_in_hub();
}

entity::Position Connection::spawn_point() const noexcept {
    // 按实体 id 在草方块上错开成网格：多名玩家重叠在同一坐标时，
    // 客户端视锥剔除会在某些视角把对方剔除，表现为"某些角度人会消失"。
    const double x = 0.5 + static_cast<double>(player_id_ % 8) * 2.0;
    const double z = 0.5 + static_cast<double>((player_id_ / 8) % 8) * 2.0;
    return entity::Position{x, 4.0, z, 0.0f, 0.0f};
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
    fields.u8(0);   // survival
    fields.i32(0);  // overworld
    fields.u8(2);   // normal
    fields.u8(static_cast<std::uint8_t>(std::min<std::int32_t>(context_.max_players, 255)));
    fields.string("default");
    fields.boolean(false);
    send_packet(proto::play_cb::kJoinGame, fields.data());
}

void Connection::send_world_state() {
    cyane::ByteWriter spawn_pos;
    spawn_pos.position(0, 4, 0);
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

    const auto spawn_chunk = world::ChunkPos::from_world(
        static_cast<std::int32_t>(player_pos_.x), static_cast<std::int32_t>(player_pos_.z));
    update_view(spawn_chunk.value_or(world::ChunkPos{0, 0}));
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

void Connection::register_in_hub() {
    if (context_.hub == nullptr) {
        return;
    }
    PlayerSnapshot snap;
    snap.entity_id = player_id_;
    snap.uuid = uuid_bytes_;
    snap.name = username_;
    snap.x = player_pos_.x;
    snap.y = player_pos_.y;
    snap.z = player_pos_.z;
    snap.yaw = player_pos_.yaw;
    snap.pitch = player_pos_.pitch;
    hub_entry_ = context_.hub->register_player(snap);
    spawn_existing_players();
    broadcast_spawn();
}

void Connection::send_chunk(world::ChunkPos pos) {
    world::Chunk chunk = world::make_flat_chunk(pos);
    cyane::ByteWriter fields;
    world::write_full_chunk(fields, chunk);
    send_packet(proto::play_cb::kChunkData, fields.data());
    loaded_chunks_.insert(chunk_key(pos));
}

void Connection::unload_chunk(world::ChunkPos pos) {
    // UnloadChunk (0x1D)：int chunkX | int chunkZ
    cyane::ByteWriter fields;
    fields.i32(pos.x);
    fields.i32(pos.z);
    send_packet(proto::play_cb::kUnloadChunk, fields.data());
    loaded_chunks_.erase(chunk_key(pos));
}

void Connection::update_view(world::ChunkPos center) {
    const std::int32_t radius = std::clamp(context_.view_distance, 2, 8);
    // 先加载视距内缺失的区块（由近及远）
    for (std::int32_t r = 0; r <= radius; ++r) {
        for (std::int32_t cx = center.x - r; cx <= center.x + r; ++cx) {
            for (std::int32_t cz = center.z - r; cz <= center.z + r; ++cz) {
                // 只处理当前环（切比雪夫距离 == r），避免重复
                const std::int32_t cheb = std::max(std::abs(cx - center.x), std::abs(cz - center.z));
                if (cheb != r) {
                    continue;
                }
                const world::ChunkPos pos{cx, cz};
                if (!loaded_chunks_.contains(chunk_key(pos))) {
                    send_chunk(pos);
                }
            }
        }
    }
    // 再卸载超出视距的区块
    std::vector<world::ChunkPos> stale;
    for (const std::int64_t key : loaded_chunks_) {
        const world::ChunkPos pos{static_cast<std::int32_t>(key >> 32),
                                  static_cast<std::int32_t>(static_cast<std::uint32_t>(key))};
        if (std::max(std::abs(pos.x - center.x), std::abs(pos.z - center.z)) > radius) {
            stale.push_back(pos);
        }
    }
    for (const world::ChunkPos pos : stale) {
        unload_chunk(pos);
    }
    last_center_ = center;
    has_center_ = true;
}

void Connection::enable_cipher(ByteSpan session_key) {
    auto decrypt_result = crypto::StreamCipher::aes_cfb8(session_key, false);
    if (!decrypt_result) {
        teardown();
        return;
    }
    decrypt_cipher_ = std::make_unique<crypto::StreamCipher>(std::move(*decrypt_result));

    auto encrypt_result = crypto::StreamCipher::aes_cfb8(session_key, true);
    if (!encrypt_result) {
        teardown();
        return;
    }
    encrypt_cipher_ = std::make_unique<crypto::StreamCipher>(std::move(*encrypt_result));
}

void Connection::flush_outbox() {
    if (!want_write_ || outbox_offset_ == 0) {
        return;
    }
    IoResult io = socket_.try_write(ByteSpan{outbox_.data(), outbox_offset_});
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
        (void)reactor_->set_writable(fd(), *this, false);
        if (close_after_flush_) {
            teardown();
        }
    }
}

void Connection::set_writable(bool writable) {
    if (writable != want_write_) {
        want_write_ = writable;
        (void)reactor_->set_writable(fd(), *this, writable);
    }
}

void Connection::send_packet(std::int32_t packet_id, ByteSpan fields) {
    if (!alive_) {
        return;
    }
    Bytes frame;
    frame.reserve(fields.size() + varint_size(static_cast<std::int32_t>(fields.size())) + varint_size(packet_id));
    proto::encode_frame(frame, packet_id, fields, compression_threshold_);
    if (frame.size() > kOutboxHighWater) {
        log::warn("connection {} outbox too large", fd());
        teardown();
        return;
    }
    if (outbox_offset_ + frame.size() > outbox_.size()) {
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
    cyane::ByteWriter fields;
    fields.string(proto::chat_text(reason));
    if (state_ == proto::State::login) {
        send_packet(proto::login_cb::kDisconnect, fields.data());
    } else if (state_ == proto::State::play) {
        send_packet(proto::play_cb::kDisconnect, fields.data());
    }
    close_after_flush_ = true;
    flush_outbox();
    if (outbox_offset_ == 0) {
        teardown();
    }
}

}