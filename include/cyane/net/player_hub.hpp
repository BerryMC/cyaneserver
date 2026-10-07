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
#include "cyane/net/packet_writers.hpp"
#include "cyane/proto/packet_ids.hpp"

namespace cyane::net {

// broadcast_near 的 exclude 参数：0 不是合法玩家实体 id（未进入 play 的连接为 0），
// 传入即"不排除任何人"——音效、方块更新等每个人都该收到的广播用它。
inline constexpr std::uint32_t kNoExclude = 0;

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
    std::uint8_t game_mode{0};
};

// 待投递给某连接的逻辑消息（未编码），由目标连接在自己的 reactor 线程取出后
// 调用自身 send_packet，从而各自正确压缩/加密。
struct HubMessage {
    std::int32_t packet_id{0};
    Bytes payload;
    bool kill_flag{false};  // true：要求目标连接自杀（用于控制台 /kill）
    std::int32_t gamemode{-1};  // ≥0：要求目标连接切换游戏模式（更新行为并回发 PlayerAbilities）
    float damage{0.0f};  // >0：生物攻击命中（扣血/受伤状态/击退），来源见 damage_from_x/z
    double damage_from_x{0.0};
    double damage_from_z{0.0};
    int experience{0};  // >0：拾取经验球累加经验
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

    // 当前在线玩家数（进入 play 阶段后才计数，status ping 不算）
    [[nodiscard]] std::size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return players_.size();
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

    // 生物攻击命中：要求目标连接扣血 + 受伤反馈（由对方 reactor 线程执行）
    bool send_damage(std::uint32_t target_id, float amount, double from_x, double from_z) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = players_.find(target_id);
        if (it == players_.end()) {
            return false;
        }
        std::lock_guard<std::mutex> mlock(it->second->mailbox_mutex);
        it->second->mailbox.push_back(
            HubMessage{0, Bytes{}, false, -1, amount, from_x, from_z});
        return true;
    }

    // 经验球拾取：要求目标连接累加经验并升级同步
    bool send_experience(std::uint32_t target_id, int amount) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = players_.find(target_id);
        if (it == players_.end()) {
            return false;
        }
        std::lock_guard<std::mutex> mlock(it->second->mailbox_mutex);
        it->second->mailbox.push_back(
            HubMessage{0, Bytes{}, false, -1, 0.0f, 0.0, 0.0, amount});
        return true;
    }

    // 向指定玩家投递一条 play 包（由对方 reactor 线程处理）
    bool send_to(std::uint32_t target_id, std::int32_t packet_id, ByteSpan payload) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = players_.find(target_id);
        if (it == players_.end()) {
            return false;
        }
        std::lock_guard<std::mutex> mlock(it->second->mailbox_mutex);
        it->second->mailbox.push_back(HubMessage{packet_id, Bytes{payload.begin(), payload.end()}});
        return true;
    }

    // 向指定玩家投递切换游戏模式指令，同时更新其快照（新玩家补发 PlayerInfo 用）
    bool send_gamemode(std::uint32_t target_id, std::uint8_t mode) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = players_.find(target_id);
        if (it == players_.end()) {
            return false;
        }
        it->second->snapshot.game_mode = mode;
        std::lock_guard<std::mutex> mlock(it->second->mailbox_mutex);
        it->second->mailbox.push_back(HubMessage{0, Bytes{}, false, static_cast<std::int32_t>(mode)});
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

    // 按玩家名查找 UUID（用于 /op、/deop）
    [[nodiscard]] std::optional<std::array<std::uint8_t, 16>> player_uuid_by_name(std::string_view name) const {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& [id, entry] : players_) {
            if (entry->snapshot.name == name) {
                return entry->snapshot.uuid;
            }
        }
        return std::nullopt;
    }

    // 获取所有在线玩家名称（用于 Tab 补全）
    [[nodiscard]] std::vector<std::string> all_player_names() const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<std::string> out;
        out.reserve(players_.size());
        for (const auto& [id, entry] : players_) {
            out.push_back(entry->snapshot.name);
        }
        return out;
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

    void transition_entity(std::int32_t old_cx, std::int32_t old_cz,
                           std::int32_t new_cx, std::int32_t new_cz,
                           std::int32_t radius,
                           std::uint32_t entity_id,
                           std::span<const std::pair<std::int32_t, Bytes>> spawn_packets) {
        std::vector<std::shared_ptr<Entry>> to_spawn;
        std::vector<std::shared_ptr<Entry>> to_destroy;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (const auto& [id, entry] : players_) {
                const auto& s = entry->snapshot;
                const std::int32_t pcx = static_cast<std::int32_t>(std::floor(s.x / 16.0));
                const std::int32_t pcz = static_cast<std::int32_t>(std::floor(s.z / 16.0));
                const bool in_old = std::max(std::abs(pcx - old_cx), std::abs(pcz - old_cz)) <= radius;
                const bool in_new = std::max(std::abs(pcx - new_cx), std::abs(pcz - new_cz)) <= radius;
                if (!in_old && in_new) {
                    to_spawn.push_back(entry);
                } else if (in_old && !in_new) {
                    to_destroy.push_back(entry);
                }
            }
        }
        for (const auto& entry : to_spawn) {
            std::lock_guard<std::mutex> lock(entry->mailbox_mutex);
            for (const auto& [pkt_id, payload] : spawn_packets) {
                entry->mailbox.push_back(HubMessage{pkt_id, payload});
            }
        }
        if (!to_destroy.empty()) {
            ByteWriter destroy;
            const std::uint32_t ids[] = {entity_id};
            writers::write_destroy_entities(destroy, ids);
            Bytes destroy_bytes{destroy.data().begin(), destroy.data().end()};
            for (const auto& entry : to_destroy) {
                std::lock_guard<std::mutex> lock(entry->mailbox_mutex);
                entry->mailbox.push_back(HubMessage{proto::play_cb::kDestroyEntities, destroy_bytes});
            }
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

    // 更新玩家快照中的游戏模式（本连接切换模式后调用，供新玩家补发 PlayerInfo 用）
    void update_game_mode(std::uint32_t entity_id, std::uint8_t mode) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (auto it = players_.find(entity_id); it != players_.end()) {
            it->second->snapshot.game_mode = mode;
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
