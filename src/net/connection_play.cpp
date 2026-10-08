#include "cyane/net/connection.hpp"

#include <algorithm>
#include <exception>
#include <array>
#include <cctype>
#include <charconv>
#include <format>
#include <string>
#include <string_view>
#include <vector>

#include "cyane/core/log.hpp"
#include "cyane/net/packet_writers.hpp"
#include "connection_detail.hpp"
#include "cyane/crypto/digest.hpp"
#include "cyane/game/command.hpp"
#include "cyane/game/op_manager.hpp"
#include "cyane/game/server.hpp"
#include "cyane/proto/json.hpp"

namespace cyane::net {

bool Connection::handle_play(std::int32_t packet_id, ByteSpan payload) {
    // 兜底：处理器内部假设被畸形包打破时抛出的异常不允许传出（terminate 整个进程）。
    // 正常路径零开销（异常只在出错时展开），出错最多断开该连接。
    try {
        return handle_play_inner(packet_id, payload);
    } catch (const std::exception& error) {
        log::warn("connection {} packet 0x{:02x} handler error: {}", fd(), packet_id, error.what());
        return false;
    }
}

bool Connection::handle_play_inner(std::int32_t packet_id, ByteSpan payload) {
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
    if (packet_id == proto::play_sb::kTabComplete) {
        return handle_tab_complete(payload);
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
    if (packet_id == proto::play_sb::kUseEntity) {
        return handle_play_use_entity(payload);
    }
    if (packet_id == proto::play_sb::kAnimation) {
        return handle_play_animation(payload);
    }
    if (packet_id == proto::play_sb::kUseItem) {
        return handle_play_use_item(payload);
    }
    // 已知但暂无游戏逻辑的 serverbound 包：静默接受，避免日志刷屏
    switch (packet_id) {
        case proto::play_sb::kSettings:                  // 客户端设置（视距/语言/皮肤部件）
        case proto::play_sb::kPluginMessage:             // 插件通道（MC|Brand 等）
        case proto::play_sb::kAbilities:                 // 飞行能力回报
        case proto::play_sb::kConfirmTransaction:        // 事务确认回执
        case proto::play_sb::kRecipeDisplayed:           // 配方书
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
    // 四种移动包（Flying/Position/PositionLook/Look）尾部都带 onGround bool
    if (reader.remaining() >= 1) {
        player_on_ground_ = reader.u8() != 0;
    }
    player_pos_ = pos;
    if (context_.player_manager != nullptr) {
        context_.player_manager->update_position(player_id_, pos);
    }
    // 掉出世界底部（虚空）致死：y < -64 触发死亡界面
    // 旁观者不会受到任何伤害（vanilla capabilities.disableDamage = true）
    if (!dead_ && pos.y < -64.0 && context_.game_mode != proto::game_mode::kSpectator) {
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
    // 位置与朝向均未变化：客户端 20Hz 常发 no-op 位移包，跳过广播避免无意义流量
    if (has_bcast_ && pos.x == last_bcast_x_ && pos.y == last_bcast_y_ && pos.z == last_bcast_z_ &&
        angle_yaw == last_bcast_yaw_ && angle_pitch == last_bcast_pitch_) {
        return;
    }
    last_bcast_x_ = pos.x;
    last_bcast_y_ = pos.y;
    last_bcast_z_ = pos.z;
    last_bcast_yaw_ = angle_yaw;
    last_bcast_pitch_ = angle_pitch;
    has_bcast_ = true;
    // 只广播给同区块视距内的玩家（远端客户端看不到这个实体）
    const auto cpos = world::ChunkPos::from_world(static_cast<std::int32_t>(pos.x),
                                                   static_cast<std::int32_t>(pos.z));
    if (!cpos) {
        return;
    }
    const std::int32_t radius = std::clamp(context_.view_distance, 2, 8);
    // EntityTeleport (0x4C)：绝对坐标最稳，避免相对移动累积误差
    ByteWriter tp;
    tp.varint(static_cast<std::int32_t>(player_id_));
    tp.f64(pos.x);
    tp.f64(pos.y);
    tp.f64(pos.z);
    tp.u8(angle_yaw);
    tp.u8(angle_pitch);
    tp.boolean(true);
    context_.hub->broadcast_near(cpos->x, cpos->z, radius, player_id_,
                                 proto::play_cb::kEntityTeleport, tp.data());
    ByteWriter head;
    head.varint(static_cast<std::int32_t>(player_id_));
    head.u8(angle_yaw);
    context_.hub->broadcast_near(cpos->x, cpos->z, radius, player_id_,
                                 proto::play_cb::kEntityHeadLook, head.data());
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

bool Connection::handle_play_animation(ByteSpan payload) {
    // 0x1D PlayerAnimation：varint hand（0 主手 1 副手）。客户端每次挥臂都会发，
    // 转发 Animation(0x06) 让附近玩家看到挥动手部（原版有 4 tick 冷却，这里 200ms 限流）
    ByteReader reader{payload};
    auto hand = reader.varint();
    if (!hand) {
        return false;
    }
    const auto now = now_ms_;
    if (now - last_anim_broadcast_ms_ < 200) {
        return true;
    }
    last_anim_broadcast_ms_ = now;
    if (context_.hub == nullptr) {
        return true;
    }
    const auto cpos = world::ChunkPos::from_world(static_cast<std::int32_t>(player_pos_.x),
                                                   static_cast<std::int32_t>(player_pos_.z));
    if (!cpos) {
        return true;
    }
    ByteWriter out;
    writers::write_animation(out, player_id_, static_cast<std::uint8_t>(*hand & 0xFF));
    const std::int32_t radius = std::clamp(context_.view_distance, 2, 8);
    context_.hub->broadcast_near(cpos->x, cpos->z, radius, player_id_,
                                 proto::play_cb::kAnimation, out.data());
    return true;
}

bool Connection::handle_play_chat(ByteSpan payload) {
    ByteReader reader{payload};
    auto maybe_message = reader.string(256);
    if (!maybe_message) {
        return false;
    }
    const std::string_view text = *maybe_message;
    log::info("<{}> {}", username_, text);

    // 玩家聊天命令：以 / 开头
    if (!text.empty() && text[0] == '/') {
        return handle_player_command(text);
    }

    // 组装 "<name> message" 聊天组件，广播给所有客户端（含自己）
    const std::string line = std::format("<{}> {}", username_, text);
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

void Connection::send_chat_feedback(std::string_view message) {
    ByteWriter chat;
    chat.string(proto::chat_text(message));
    chat.u8(0);  // position：0 = 聊天框
    send_packet(proto::play_cb::kChatMessage, chat.data());
}

class PlayerCommandSender : public game::CommandSender {
public:
    PlayerCommandSender(Connection& conn, std::string_view name, std::uint8_t op,
                        const entity::Position& pos, std::uint32_t id)
        : conn_{conn}, name_{name}, op_{op}, pos_{pos}, id_{id} {}

    [[nodiscard]] std::string_view name() const noexcept override { return name_; }
    [[nodiscard]] bool is_player() const noexcept override { return true; }
    [[nodiscard]] std::uint8_t op_level() const noexcept override { return op_; }
    [[nodiscard]] const entity::Position* player_position() const noexcept override { return &pos_; }
    [[nodiscard]] std::uint32_t player_entity_id() const noexcept override { return id_; }
    void send_feedback(std::string_view message, bool is_error = false) override {
        if (is_error) {
            conn_.send_chat_feedback(std::format("§c{}", message));
        } else {
            conn_.send_chat_feedback(message);
        }
    }

private:
    Connection& conn_;
    std::string_view name_;
    std::uint8_t op_;
    const entity::Position& pos_;
    std::uint32_t id_;
};

bool Connection::handle_player_command(std::string_view text) {
    if (context_.server == nullptr) {
        return true;
    }
    const std::uint8_t op = context_.op_manager != nullptr ? context_.op_manager->op_level(uuid_.dashed()) : 0;
    PlayerCommandSender sender{*this, username_, op, player_pos_, player_id_};
    return game::CommandDispatcher::execute(sender, *context_.server, text);
}


void Connection::set_game_mode(std::uint8_t mode) {
    context_.game_mode = mode;
    if (context_.hub != nullptr) {
        context_.hub->update_game_mode(player_id_, mode);
    }
    // ChangeGameState (0x1E)：byte state(3=模式变更) | float 模式值。1.12.2 客户端据此
    // 更新本地 GameType（血条/饥饿条显隐、创造物品栏）；PlayerInfo 只更新 Tab 栏
    ByteWriter gs;
    gs.u8(3);
    gs.f32(static_cast<float>(mode));
    send_packet(proto::play_cb::kGameStateChange, gs.data());
    // PlayerInfo (0x2E) 更新游戏模式（客户端据此显示旁观者/创造UI）
    ByteWriter info;
    detail::write_player_info_game_mode(info, uuid_.bytes(), mode);
    if (context_.hub != nullptr) {
        context_.hub->broadcast_all(proto::play_cb::kPlayerInfo, info.data());
    } else {
        send_packet(proto::play_cb::kPlayerInfo, info.data());
    }
    send_abilities_for(mode);
}

// 远端切换（/gamemode <他人>）：hub 投递到目标连接的 reactor 线程执行，
// 保证目标的行为判定（挖掘/放置/飞行）与显示一并更新
void Connection::apply_remote_gamemode(std::uint8_t mode) {
    context_.game_mode = mode;
    if (context_.hub != nullptr) {
        context_.hub->update_game_mode(player_id_, mode);
    }
    ByteWriter gs;
    gs.u8(3);
    gs.f32(static_cast<float>(mode));
    send_packet(proto::play_cb::kGameStateChange, gs.data());
    send_abilities_for(mode);
}

void Connection::send_abilities_for(std::uint8_t mode) {
    ByteWriter abilities;
    switch (mode) {
        case proto::game_mode::kCreative:
            abilities.u8(proto::abilities::kInvulnerable | proto::abilities::kAllowFlying |
                         proto::abilities::kCreativeMode);
            abilities.f32(0.05f);
            abilities.f32(0.1f);
            break;
        case proto::game_mode::kSpectator:
            abilities.u8(proto::abilities::kInvulnerable | proto::abilities::kAllowFlying |
                         proto::abilities::kFlying);
            abilities.f32(0.1f);
            abilities.f32(0.0f);
            break;
        default:  // survival / adventure
            abilities.u8(0);
            abilities.f32(0.05f);
            abilities.f32(0.1f);
            break;
    }
    send_packet(proto::play_cb::kPlayerAbilities, abilities.data());
}

bool Connection::handle_tab_complete(ByteSpan payload) {
    // 1.12.2 serverbound TabComplete (0x01) 格式：
    //   text: string
    //   assumeCommand: bool
    //   lookedAtBlock?: position
    // 注意：不含 transaction_id
    ByteReader reader{payload};
    const auto text = reader.string();
    if (!text) {
        log::warn("connection {} tab complete text read failed", fd());
        return false;
    }
    const auto assume_command = reader.boolean();
    if (!assume_command) {
        log::warn("connection {} tab complete assumeCommand read failed", fd());
        return false;
    }
    // 1.12.2：hasLookedAtBlock: bool，为 true 时随后是 8 字节打包 position
    if (reader.remaining() > 0) {
        const auto has_looked = reader.boolean();
        if (has_looked && *has_looked) {
            const auto packed = reader.i64();
            if (packed) {
                log::debug("connection {} tab complete lookedAtBlock: {}, {}, {}", fd(),
                           position_x(*packed), position_y(*packed), position_z(*packed));
            }
        }
    }

    std::vector<std::string> matches;
    // 1.12.2：视线对着方块时客户端会把 assumeCommand 置 false（仍带 lookedAtBlock），
    // 因此命令补全以文本是否以命令符 / 开头为准，或客户端显式声明命令上下文
    const bool command_mode =
        *assume_command || (!text->empty() && text->at(0) == '/');

    if (command_mode) {
        if (context_.server != nullptr) {
            const std::uint8_t op = context_.op_manager != nullptr ? context_.op_manager->op_level(uuid_.dashed()) : 0;
            PlayerCommandSender sender{*this, username_, op, player_pos_, player_id_};
            matches = game::CommandDispatcher::tab_complete(sender, *context_.server, *text);
        }
    } else {
        // 聊天玩家名补全（assumeCommand=false）：不区分大小写的前缀匹配
        const std::string lower_text = [&text] {
            std::string s{*text};
            std::transform(s.begin(), s.end(), s.begin(),
                           [](unsigned char c) { return std::tolower(c); });
            return s;
        }();
        if (context_.hub != nullptr) {
            for (const auto& name : context_.hub->all_player_names()) {
                std::string lower_name{name};
                std::transform(lower_name.begin(), lower_name.end(), lower_name.begin(),
                               [](unsigned char c) { return std::tolower(c); });
                if (lower_name.starts_with(lower_text)) {
                    matches.push_back(name);
                }
            }
        }
    }

    // 发送 TabComplete 响应 (0x0E)
    // 1.12.2 clientbound 格式：matches: string[]varint
    // 不含 transaction_id，也不含 has_tooltip 字段
    ByteWriter out;
    out.varint(static_cast<std::int32_t>(matches.size()));
    for (const auto& m : matches) {
        out.string(m);
    }
    send_packet(proto::play_cb::kTabComplete, out.data());
    return true;
}

}
