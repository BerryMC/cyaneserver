#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "cyane/entity/player_entity.hpp"

namespace cyane::entity {

// 实体 ID 0 保留，玩家与非玩家实体共用同一计数器以保证全局唯一
[[nodiscard]] inline std::uint32_t allocate_entity_id() noexcept {
    static std::atomic<std::uint32_t> next{1};
    return next.fetch_add(1, std::memory_order_relaxed);
}

class PlayerManager {
public:
    using PlayerList = std::vector<std::shared_ptr<Player>>;

    PlayerManager() = default;

    std::uint32_t add(std::uint32_t id, std::string name, Position pos) {
        std::lock_guard<std::mutex> lock(mutex_);
        players_[id] = std::make_shared<Player>(id, std::move(name), std::move(pos));
        return id;
    }

    std::shared_ptr<Player> get(std::uint32_t id) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = players_.find(id);
        if (it != players_.end()) {
            return it->second;
        }
        return nullptr;
    }

    void remove(std::uint32_t id) {
        std::lock_guard<std::mutex> lock(mutex_);
        players_.erase(id);
    }

    PlayerList get_all_copy() const {
        std::lock_guard<std::mutex> lock(mutex_);
        PlayerList result;
        result.reserve(players_.size());
        for (const auto& [id, player] : players_) {
            result.push_back(player);
        }
        return result;
    }

    void update_position(std::uint32_t id, Position pos) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = players_.find(id);
        if (it != players_.end()) {
            it->second->set_position(std::move(pos));
        }
    }

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::uint32_t, std::shared_ptr<Player>> players_;
};

}
