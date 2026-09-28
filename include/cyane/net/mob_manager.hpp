#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <vector>

#include "cyane/entity/player_entity.hpp"

namespace cyane::net {

// 角度 float(度) → 1/256 圈的字节角（协议线格式）
[[nodiscard]] inline std::uint8_t angle_byte(float degrees) noexcept {
    return static_cast<std::uint8_t>(static_cast<int>(degrees * 256.0f / 360.0f) & 0xFF);
}

// 被动生物：位置 + 朝向 + 漫游 AI 状态
struct Mob {
    std::uint32_t entity_id{0};
    std::int32_t type{90};  // 1.12.2 SpawnMob 类型：90 猪 91 羊 92 牛 93 鸡
    entity::Position pos{};
    // AI 状态
    bool walking{false};
    std::int32_t state_ticks{0};  // 当前状态剩余 tick
    double dir_x{0.0};
    double dir_z{0.0};
};

// 生物登记表：Server::tick 驱动 AI（20Hz），连接线程读快照补发。
// 移动广播由 tick 返回的"本 tick 移动列表"驱动（Server 持 hub）。
class MobManager {
public:
    // 出生点半径内随机生成 count 只被动生物
    void spawn_passive(std::size_t count);

    // 推进 AI 一个 tick，返回本 tick 发生移动的生物
    [[nodiscard]] std::vector<Mob> tick();

    [[nodiscard]] std::vector<Mob> snapshot() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return mobs_;
    }

    [[nodiscard]] std::optional<Mob> by_id(std::uint32_t id) const {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& m : mobs_) {
            if (m.entity_id == id) {
                return m;
            }
        }
        return std::nullopt;
    }

    // 移除生物（返回 true 表示存在且已移除）
    bool remove(std::uint32_t id) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (std::size_t i = 0; i < mobs_.size(); ++i) {
            if (mobs_[i].entity_id == id) {
                mobs_[i] = mobs_.back();
                mobs_.pop_back();
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] std::size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return mobs_.size();
    }

    static constexpr double kWanderSpeed = 0.05;    // 格/tick，约 1 格/秒
    static constexpr double kWanderRadius = 24.0;   // 距出生点最大半径
    static constexpr double kSpawnY = 4.0;          // 超平坦草地表面

private:
    mutable std::mutex mutex_;
    std::vector<Mob> mobs_;
};

}
