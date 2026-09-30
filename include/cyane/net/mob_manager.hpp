#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <span>
#include <vector>

#include "cyane/entity/player_entity.hpp"
#include "cyane/entity/player_manager.hpp"

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

// 持久化形状：类型 + 位置 + 朝向（存档 Entities 列表的最小字段集）
struct MobState {
    std::uint8_t type{90};
    double x{0.0};
    double y{0.0};
    double z{0.0};
    float yaw{0.0f};
    float pitch{0.0f};
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

    // 从存档恢复生物（分配新实体 id，AI 状态重置为待机）
    void restore(std::span<const MobState> restored) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& s : restored) {
            Mob m;
            m.entity_id = entity::allocate_entity_id();
            m.type = s.type;
            m.pos.x = s.x;
            m.pos.y = s.y;
            m.pos.z = s.z;
            m.pos.yaw = s.yaw;
            m.pos.pitch = s.pitch;
            mobs_.push_back(std::move(m));
        }
    }

    // 全量快照（保存存档用）
    [[nodiscard]] std::vector<MobState> all_mobs() const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<MobState> out;
        out.reserve(mobs_.size());
        for (const auto& m : mobs_) {
            out.push_back(MobState{static_cast<std::uint8_t>(m.type), m.pos.x, m.pos.y, m.pos.z,
                                   m.pos.yaw, m.pos.pitch});
        }
        return out;
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
