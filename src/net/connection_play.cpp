#include "cyane/net/connection.hpp"

#include <format>

#include "cyane/core/log.hpp"
#include "connection_detail.hpp"
#include "cyane/proto/json.hpp"

namespace cyane::net {

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
    if (packet_id == proto::play_sb::kPlayerDigging) {
        return handle_play_digging(payload);
    }
    if (packet_id == proto::play_sb::kBlockPlace) {
        return handle_play_block_place(payload);
    }
    if (packet_id == proto::play_sb::kHeldItemChange) {
        return handle_play_held_item(payload);
    }
    if (packet_id == proto::play_sb::kCreativeInventoryAction) {
        return handle_play_creative_action(payload);
    }
    if (packet_id == proto::play_sb::kClickWindow) {
        return handle_play_click_window(payload);
    }
    if (packet_id == proto::play_sb::kClientCommand) {
        return handle_play_client_command(payload);
    }
    if (packet_id == proto::play_sb::kCloseWindow) {
        return handle_play_close_window(payload);
    }
    // 已知但暂无游戏逻辑的 serverbound 包：静默接受，避免日志刷屏
    switch (packet_id) {
        case proto::play_sb::kSettings:                  // 客户端设置（视距/语言/皮肤部件）
        case proto::play_sb::kPluginMessage:             // 插件通道（MC|Brand 等）
        case proto::play_sb::kAbilities:                 // 飞行能力回报
        case proto::play_sb::kAnimation:                 // 挥手动画
        case proto::play_sb::kConfirmTransaction:        // 事务确认回执
        case proto::play_sb::kRecipeDisplayed:           // 配方书
        case proto::play_sb::kUseItem:                   // 使用物品
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
    // 掉出世界底部（虚空）致死：y < -64 触发死亡界面
    if (!dead_ && pos.y < -64.0) {
        kill_player();
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
    const std::uint8_t angle_yaw = detail::to_angle_byte(pos.yaw);
    const std::uint8_t angle_pitch = detail::to_angle_byte(pos.pitch);
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

}
