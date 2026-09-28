#include "cyane/net/mob_manager.hpp"

#include <cmath>
#include <random>

namespace cyane::net {
namespace {

// 1.12.2 被动生物类型 id
constexpr std::array<std::int32_t, 4> kPassiveTypes = {90, 91, 92, 93};  // 猪 羊 牛 鸡

std::uint32_t default_next_id() {
    static std::atomic<std::uint32_t> next{1000};  // 与玩家实体 id 空间错开
    return next.fetch_add(1, std::memory_order_relaxed);
}

std::mt19937& rng() {
    thread_local std::mt19937 engine{std::random_device{}()};
    return engine;
}

std::uniform_real_distribution<double> dist01{0.0, 1.0};

}  // namespace

void MobManager::spawn_passive(std::size_t count) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto& random = rng();
    for (std::size_t i = 0; i < count; ++i) {
        Mob m;
        m.entity_id = default_next_id();
        m.type = kPassiveTypes[static_cast<std::size_t>(dist01(random) * 3.999)];
        // 出生点半径 4..kWanderRadius 内随机
        const double angle = dist01(random) * 6.28318530718;
        const double radius = 4.0 + dist01(random) * (kWanderRadius - 4.0);
        m.pos.x = 0.5 + std::cos(angle) * radius;
        m.pos.z = 0.5 + std::sin(angle) * radius;
        m.pos.y = kSpawnY;
        m.pos.yaw = static_cast<float>(dist01(random) * 360.0);
        m.walking = false;
        m.state_ticks = static_cast<std::int32_t>(dist01(random) * 100.0) + 20;
        mobs_.push_back(m);
    }
}

std::vector<Mob> MobManager::tick() {
    std::vector<Mob> moved;
    std::lock_guard<std::mutex> lock(mutex_);
    auto& random = rng();
    for (auto& m : mobs_) {
        if (m.state_ticks > 0) {
            --m.state_ticks;
        }
        if (m.state_ticks == 0) {
            // 状态切换：60% 概率开始漫游，否则停留 1-4 秒
            if (dist01(random) < 0.6) {
                const double angle = dist01(random) * 6.28318530718;
                m.dir_x = std::cos(angle);
                m.dir_z = std::sin(angle);
                m.walking = true;
                m.state_ticks = static_cast<std::int32_t>(dist01(random) * 60.0) + 20;
            } else {
                m.walking = false;
                m.state_ticks = static_cast<std::int32_t>(dist01(random) * 80.0) + 20;
            }
        }
        if (!m.walking) {
            continue;
        }
        // 出生点边界回拉
        const double dist_sq = m.pos.x * m.pos.x + m.pos.z * m.pos.z;
        if (dist_sq > kWanderRadius * kWanderRadius) {
            const double back = std::sqrt(dist_sq);
            m.dir_x = -m.pos.x / back;
            m.dir_z = -m.pos.z / back;
            m.pos.yaw = static_cast<float>(std::atan2(-m.dir_x, m.dir_z) * 180.0 / 3.14159265358979);
        }
        m.pos.x += m.dir_x * kWanderSpeed;
        m.pos.z += m.dir_z * kWanderSpeed;
        if (m.walking && m.state_ticks % 10 == 0) {
            // 移动中缓慢更新朝向（每 0.5s 与方向同步）
            m.pos.yaw = static_cast<float>(std::atan2(-m.dir_x, m.dir_z) * 180.0 / 3.14159265358979);
        }
        moved.push_back(m);
    }
    return moved;
}

}
