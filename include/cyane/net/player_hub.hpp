#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "cyane/core/bytes.hpp"

namespace cyane::net {

// 玩家在广播中心里的一条位置/身份快照
struct PlayerSnapshot {
    std::uint32_t entity_id{0};
    std::array<std::uint8_t, 16> uuid{};
    std::string name;
    double x{0.0};
    double y{0.0};
    double z{0.0};
    float yaw{0.0f};
    float pitch{0.0f};
};

// 待投递给某连接的逻辑消息（未编码），由目标连接在自己的 reactor 线程取出后
// 调用自身 send_packet，从而各自正确压缩/加密。
struct HubMessage {
    std::int32_t packet_id{0};
    Bytes payload;
    bool kill_flag{false};  // true：要求目标连接自杀（用于控制台 /kill）
};

// 线程安全的多人广播中心：连接跨 reactor 线程时也安全。
// - register/unregister 维护在线玩家表
// - 广播只把逻辑消息塞进各连接的 mailbox（带锁），不直接触碰对端 socket
class PlayerHub {
public:
    // 每个在线玩家一份：身份快照 + 自己的收件箱
    struct Entry {
        PlayerSnapshot snapshot;
        std::mutex mailbox_mutex;
        std::vector<HubMessage> mailbox;
    };

    // 登记新玩家，返回其 Entry（连接持有以取自己的收件箱）
    [[nodiscard]] std::shared_ptr<Entry> register_player(const PlayerSnapshot& snap) {
        auto entry = std::make_shared<Entry>();
        entry->snapshot = snap;
        std::lock_guard<std::mutex> lock(mutex_);
        players_[snap.entity_id] = entry;
        return entry;
    }

    void unregister_player(std::uint32_t entity_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        players_.erase(entity_id);
    }

    // 除 exclude 外的所有在线玩家快照（新玩家登录时补发已有玩家用）
    [[nodiscard]] std::vector<PlayerSnapshot> others(std::uint32_t exclude) const {
        std::vector<PlayerSnapshot> out;
        std::lock_guard<std::mutex> lock(mutex_);
        out.reserve(players_.size());
        for (const auto& [id, entry] : players_) {
            if (id != exclude) {
                out.push_back(entry->snapshot);
            }
        }
        return out;
    }

    // 向除 exclude 外的所有玩家投递一条消息
    void broadcast(std::uint32_t exclude, std::int32_t packet_id, ByteSpan payload) {
        std::vector<std::shared_ptr<Entry>> targets;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            targets.reserve(players_.size());
            for (const auto& [id, entry] : players_) {
                if (id != exclude) {
                    targets.push_back(entry);
                }
            }
        }
        for (const auto& entry : targets) {
            std::lock_guard<std::mutex> lock(entry->mailbox_mutex);
            entry->mailbox.push_back(HubMessage{packet_id, Bytes{payload.begin(), payload.end()}});
        }
    }

    // exclude=0 意味着不排除任何人（entity id 从 1 起）；用于聊天等全员广播
    void broadcast_all(std::int32_t packet_id, ByteSpan payload) { broadcast(0, packet_id, payload); }

    // 向指定玩家投递一条带 kill 标记的消息（由对方 reactor 线程处理）
    bool send_kill(std::uint32_t target_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = players_.find(target_id);
        if (it == players_.end()) {
            return false;
        }
        std::lock_guard<std::mutex> mlock(it->second->mailbox_mutex);
        it->second->mailbox.push_back(HubMessage{/*packet_id=*/0, Bytes{}, /*kill_flag=*/true});
        return true;
    }

    // 按玩家名查找 entity id（用于控制台 /kill 等）
    [[nodiscard]] std::uint32_t player_id_by_name(std::string_view name) const {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& [id, entry] : players_) {
            if (entry->snapshot.name == name) {
                return id;
            }
        }
        return 0;
    }

    // 只投递给所在区块与 (cx,cz) 的切比雪夫距离 ≤ radius 的玩家（方块变更等局部事件）
    void broadcast_near(std::int32_t cx, std::int32_t cz, std::int32_t radius, std::uint32_t exclude,
                        std::int32_t packet_id, ByteSpan payload) {
        std::vector<std::shared_ptr<Entry>> targets;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            targets.reserve(players_.size());
            for (const auto& [id, entry] : players_) {
                if (id == exclude) {
                    continue;
                }
                const auto& s = entry->snapshot;
                const std::int32_t pcx = static_cast<std::int32_t>(std::floor(s.x / 16.0));
                const std::int32_t pcz = static_cast<std::int32_t>(std::floor(s.z / 16.0));
                if (std::max(std::abs(pcx - cx), std::abs(pcz - cz)) <= radius) {
                    targets.push_back(entry);
                }
            }
        }
        for (const auto& entry : targets) {
            std::lock_guard<std::mutex> lock(entry->mailbox_mutex);
            entry->mailbox.push_back(HubMessage{packet_id, Bytes{payload.begin(), payload.end()}});
        }
    }

    void update_position(std::uint32_t entity_id, double x, double y, double z, float yaw, float pitch) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (auto it = players_.find(entity_id); it != players_.end()) {
            auto& s = it->second->snapshot;
            s.x = x;
            s.y = y;
            s.z = z;
            s.yaw = yaw;
            s.pitch = pitch;
        }
    }

    // 检查在 (bx,by,bz) 放置实心方块是否会与任何在线玩家的碰撞体重叠。
    // 玩家近似为 0.6x0.6 水平截面、1.8 高的 AABB，脚下为 y..y+1.8。
    [[nodiscard]] bool block_intersects_any_player(std::int32_t bx, std::int32_t by,
                                                    std::int32_t bz) const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& [id, entry] : players_) {
            const auto& s = entry->snapshot;
            const double px = s.x;
            const double py = s.y;
            const double pz = s.z;
            // 水平方向：玩家 AABB [px-0.3, px+0.3] 与方块 [bx, bx+1) 是否相交
            if (px + 0.3 > static_cast<double>(bx) && px - 0.3 < static_cast<double>(bx + 1) &&
                pz + 0.3 > static_cast<double>(bz) && pz - 0.3 < static_cast<double>(bz + 1) &&
                py + 1.8 > static_cast<double>(by) && py < static_cast<double>(by + 1)) {
                return true;
            }
        }
        return false;
    }

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::uint32_t, std::shared_ptr<Entry>> players_;
};

}
