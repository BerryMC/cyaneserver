#include "cyane/net/connection.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <format>
#include <string>
#include <string_view>
#include <vector>

#include "cyane/core/log.hpp"
#include "connection_detail.hpp"
#include "cyane/crypto/digest.hpp"
#include "cyane/game/op_manager.hpp"
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
    // Animation (0x06)：varint entityId | byte hand
    ByteWriter out;
    out.varint(static_cast<std::int32_t>(player_id_));
    out.u8(static_cast<std::uint8_t>(*hand & 0xFF));
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

// 解析命令词和参数
static std::pair<std::string_view, std::vector<std::string_view>> split_command(std::string_view text) {
    // 去掉前导 /
    if (!text.empty() && text[0] == '/') {
        text.remove_prefix(1);
    }
    // 第一个词是命令名
    const auto space = text.find_first_of(" \t");
    std::string_view cmd = text.substr(0, space);
    std::vector<std::string_view> args;
    if (space != std::string_view::npos) {
        std::string_view rest = text.substr(space + 1);
        while (!rest.empty()) {
            // 跳过分隔符
            while (!rest.empty() && (rest[0] == ' ' || rest[0] == '\t')) {
                rest.remove_prefix(1);
            }
            if (rest.empty()) break;
            const auto next = rest.find_first_of(" \t");
            args.push_back(rest.substr(0, next));
            if (next == std::string_view::npos) break;
            rest = rest.substr(next + 1);
        }
    }
    return {cmd, args};
}

// 游戏模式名称 → 数值
static std::optional<std::uint8_t> parse_game_mode(std::string_view name) {
    if (name == "survival" || name == "0") return proto::game_mode::kSurvival;
    if (name == "creative" || name == "1") return proto::game_mode::kCreative;
    if (name == "adventure" || name == "2") return proto::game_mode::kAdventure;
    if (name == "spectator" || name == "3") return proto::game_mode::kSpectator;
    return std::nullopt;
}

// 游戏模式数值 → 名称
static std::string_view game_mode_name(std::uint8_t mode) {
    switch (mode) {
        case proto::game_mode::kSurvival:  return "survival";
        case proto::game_mode::kCreative:  return "creative";
        case proto::game_mode::kAdventure: return "adventure";
        case proto::game_mode::kSpectator: return "spectator";
        default: return "unknown";
    }
}

bool Connection::handle_player_command(std::string_view text) {
    const auto [cmd, args] = split_command(text);
    if (cmd.empty()) {
        return true;
    }
    const std::uint8_t op = context_.op_manager != nullptr ? context_.op_manager->op_level(uuid_.dashed()) : 0;

    if (cmd == "help") {
        send_chat_feedback("可用命令: /gamemode /tp /kill /op /deop /say /tps /help");
        return true;
    }
    if (cmd == "gamemode") {
        if (args.empty()) {
            send_chat_feedback("用法: /gamemode <survival|creative|adventure|spectator> [玩家]");
            return true;
        }
        const auto mode = parse_game_mode(args[0]);
        if (!mode) {
            send_chat_feedback("未知模式: " + std::string(args[0]));
            return true;
        }
        std::string_view target_name = username_;
        if (args.size() >= 2) {
            if (op < 2) {
                send_chat_feedback("§c权限不足");
                return true;
            }
            target_name = args[1];
        }
        if (target_name == username_) {
            set_game_mode(*mode);
            send_chat_feedback(std::format("游戏模式已切换为 {}", game_mode_name(*mode)));
        } else {
            if (context_.hub == nullptr) {
                send_chat_feedback("玩家不在线: " + std::string(target_name));
                return true;
            }
            const auto target_id = context_.hub->player_id_by_name(target_name);
            if (target_id == 0) {
                send_chat_feedback("玩家不在线: " + std::string(target_name));
                return true;
            }
            // 目标玩家的 uuid 广播其模式变更（Tab 栏条目按 uuid 匹配）
            const auto target_uuid = context_.hub->player_uuid_by_name(target_name);
            if (target_uuid) {
                ByteWriter info;
                detail::write_player_info_game_mode(info, *target_uuid, *mode);
                context_.hub->broadcast_all(proto::play_cb::kPlayerInfo, info.data());
            }
            // 让目标连接更新自身行为判定并发 PlayerAbilities
            context_.hub->send_gamemode(target_id, *mode);
            send_chat_feedback(std::format("{} 的游戏模式已切换为 {}", target_name, game_mode_name(*mode)));
        }
        return true;
    }
    if (cmd == "tp") {
        if (args.empty()) {
            send_chat_feedback("用法: /tp <玩家>");
            return true;
        }
        if (op < 2) {
            send_chat_feedback("§c权限不足");
            return true;
        }
        if (context_.hub == nullptr) {
            send_chat_feedback("玩家不在线: " + std::string(args[0]));
            return true;
        }
        const auto others = context_.hub->others(player_id_);
        for (const auto& other : others) {
            if (other.name == args[0]) {
                player_pos_.x = other.x;
                player_pos_.y = other.y;
                player_pos_.z = other.z;
                ++teleport_id_;
                cyane::ByteWriter tp;
                tp.f64(other.x);
                tp.f64(other.y);
                tp.f64(other.z);
                tp.f32(player_pos_.yaw);
                tp.f32(player_pos_.pitch);
                tp.u8(0);
                tp.varint(teleport_id_);
                send_packet(proto::play_cb::kPlayerPositionLook, tp.data());
                send_chat_feedback(std::format("已传送到 {}", args[0]));
                return true;
            }
        }
        send_chat_feedback("玩家不在线: " + std::string(args[0]));
        return true;
    }
    if (cmd == "kill") {
        if (args.empty()) {
            kill_player();
        } else {
            if (op < 2) {
                send_chat_feedback("§c权限不足");
                return true;
            }
            if (context_.hub == nullptr) {
                send_chat_feedback("玩家不在线: " + std::string(args[0]));
                return true;
            }
            const auto target_id = context_.hub->player_id_by_name(args[0]);
            if (target_id == 0) {
                send_chat_feedback("玩家不在线: " + std::string(args[0]));
                return true;
            }
            context_.hub->send_kill(target_id);
            send_chat_feedback(std::format("已杀死 {}", args[0]));
        }
        return true;
    }
    if (cmd == "say") {
        if (op < 2) {
            send_chat_feedback("§c权限不足");
            return true;
        }
        if (args.empty()) {
            send_chat_feedback("用法: /say <消息>");
            return true;
        }
        std::string msg = "[Server] ";
        for (std::size_t i = 0; i < args.size(); ++i) {
            if (i > 0) msg += ' ';
            msg += args[i];
        }
        ByteWriter chat;
        chat.string(proto::chat_text(msg));
        chat.u8(0);
        if (context_.hub != nullptr) {
            context_.hub->broadcast_all(proto::play_cb::kChatMessage, chat.data());
        }
        return true;
    }
    if (cmd == "tps") {
        const double tps = context_.tick_stats != nullptr ? context_.tick_stats->tps() : 0.0;
        send_chat_feedback(std::format("TPS: {:.1f}", tps));
        return true;
    }
    if (cmd == "op") {
        if (args.empty()) {
            send_chat_feedback("用法: /op <玩家>");
            return true;
        }
        if (op < 4) {
            send_chat_feedback("§c权限不足");
            return true;
        }
        if (context_.hub == nullptr) {
            send_chat_feedback("玩家不在线: " + std::string(args[0]));
            return true;
        }
        const auto uuid_opt = context_.hub->player_uuid_by_name(args[0]);
        if (!uuid_opt) {
            send_chat_feedback("玩家不在线: " + std::string(args[0]));
            return true;
        }
        const std::string target_uuid = Uuid::from_bytes(*uuid_opt).dashed();
        (void)context_.op_manager->op_player(target_uuid, args[0]);
        send_chat_feedback(std::format("已将 {} 设为 OP", args[0]));
        return true;
    }
    if (cmd == "deop") {
        if (args.empty()) {
            send_chat_feedback("用法: /deop <玩家>");
            return true;
        }
        if (op < 4) {
            send_chat_feedback("§c权限不足");
            return true;
        }
        if (context_.hub == nullptr) {
            send_chat_feedback("玩家不在线: " + std::string(args[0]));
            return true;
        }
        const auto uuid_opt = context_.hub->player_uuid_by_name(args[0]);
        if (!uuid_opt) {
            send_chat_feedback("玩家不在线: " + std::string(args[0]));
            return true;
        }
        const std::string target_uuid = Uuid::from_bytes(*uuid_opt).dashed();
        (void)context_.op_manager->deop_player(target_uuid);
        send_chat_feedback(std::format("已撤销 {} 的 OP 权限", args[0]));
        return true;
    }
    send_chat_feedback("未知命令: /" + std::string(cmd));
    return true;
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
        // 命令补全
        const std::string cmd_text = text->substr(1); // 去掉 /
        // 找到命令名
        const auto space = cmd_text.find_first_of(" \t");
        std::string cmd_name = cmd_text.substr(0, space);
        std::string arg_text = "";
        if (space != std::string::npos) {
            arg_text = cmd_text.substr(space + 1);
        }

        if (space == std::string::npos) {
            // 还没有空格：补全命令名本身
            static constexpr std::array commands = {
                "gamemode", "tp", "kill", "op", "deop", "help", "say", "stop"
            };
            for (const auto& c : commands) {
                if (std::string_view(c).starts_with(cmd_name)) {
                    matches.push_back(std::format("/{}", c));
                }
            }
        } else if (cmd_name == "gamemode") {
            // 补全模式名
            static constexpr std::array modes = {
                "survival", "creative", "adventure", "spectator"
            };
            for (const auto& m : modes) {
                if (std::string_view(m).starts_with(arg_text)) {
                    matches.push_back(std::string(m));
                }
            }
        } else if (cmd_name == "tp" || cmd_name == "op" || cmd_name == "deop") {
            // 补全玩家名
            if (context_.hub != nullptr) {
                const auto players = context_.hub->all_player_names();
                for (const auto& name : players) {
                    if (std::string_view(name).starts_with(arg_text)) {
                        matches.push_back(name);
                    }
                }
            }
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
