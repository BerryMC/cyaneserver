#include "cyane/net/connection.hpp"

#include "cyane/core/log.hpp"

namespace cyane::net {

// 登录时恢复玩家数据：位置、朝向、游戏模式、血量与 46 格背包
void Connection::load_player_data() {
    if (context_.player_data_store == nullptr || uuid_.is_null()) {
        return;
    }
    // player_pos_ 已由 spawn_point()（含地表探测）预置，作为新玩家默认位置传入
    const auto data = context_.player_data_store->load_or_default(
        uuid_.dashed(), username_, context_.game_mode, player_pos_.x, player_pos_.y, player_pos_.z);
    player_pos_.x = data.x;
    player_pos_.y = data.y;
    player_pos_.z = data.z;
    player_pos_.yaw = data.yaw;
    player_pos_.pitch = data.pitch;
    health_ = data.health;
    dead_ = false;
    if (data.game_mode != context_.game_mode) {
        context_.game_mode = data.game_mode;
        if (context_.hub != nullptr) {
            context_.hub->update_game_mode(player_id_, data.game_mode);
        }
    }
    for (std::size_t index = 0; index < inventory_.slots().size(); ++index) {
        inventory_.slots()[index] = data.inventory[index];
    }
    log::debug("{}: restored player data from {}", username_, uuid_.dashed());
}

// 断开时把玩家数据落盘
void Connection::save_player_data() {
    if (context_.player_data_store == nullptr || state_ != proto::State::play || uuid_.is_null()) {
        return;
    }
    game::PlayerData data;
    data.username = username_;
    data.uuid_with_dashes = uuid_.dashed();
    data.x = player_pos_.x;
    data.y = player_pos_.y;
    data.z = player_pos_.z;
    data.yaw = player_pos_.yaw;
    data.pitch = player_pos_.pitch;
    data.game_mode = context_.game_mode;
    data.health = health_;
    data.selected_slot = selected_slot_;
    data.inventory = inventory_.slots();
    if (const auto saved = context_.player_data_store->save(data); !saved) {
        log::warn("{}: cannot save player data: {}", username_, saved.error().message);
    }
}

}
