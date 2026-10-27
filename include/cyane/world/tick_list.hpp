#pragma once

#include <cstdint>
#include <functional>
#include <set>
#include <unordered_map>
#include <vector>

namespace cyane::world {

// 1.12.2 原版延迟方块更新条目（NextTickListEntry 逐行转写）
// 排序键依次为：到期时间 → 优先级 → 插入序号（同一时刻的多个更新按调度顺序执行）
struct NextTickListEntry {
    int x{0};
    int y{0};
    int z{0};
    std::uint16_t block_id{0};
    std::int64_t scheduled_time{0};
    int priority{0};
    std::uint64_t tick_entry_id{0};
};

// 1.12.2 原版区块延迟更新队列（TickList 逐行转写）
// 排序：scheduled_time → priority → tick_entry_id；去重键：(位置, 方块)
// vanilla 单 tick 最多处理 1000 条（MAX_TICK），避免单区块积压卡死主线程
class TickList {
public:
    static constexpr int kMaxTick = 1000;

    // 已存在同目标条目则整体替换其到期时间（vanilla：命中后先移除再按新时间插入）
    void schedule(int x, int y, int z, std::uint16_t block_id, std::int64_t scheduled_time) {
        const TargetKey target{x, y, z, block_id};
        if (const auto it = index_.find(target); it != index_.end()) {
            if (it->second->scheduled_time == scheduled_time) return;
            ordered_.erase(it->second);
            index_.erase(it);
        }
        const NextTickListEntry entry{x, y, z, block_id, scheduled_time, 0, next_id_++};
        const auto pos = ordered_.insert(entry).first;
        index_.emplace(target, pos);
    }

    [[nodiscard]] bool is_scheduled(int x, int y, int z, std::uint16_t block_id) const noexcept {
        return index_.contains(TargetKey{x, y, z, block_id});
    }

    // 执行所有到期条目（callback 内允许继续 schedule，与 vanilla tempTickList 语义一致）
    void tick(std::int64_t now, const std::function<void(const NextTickListEntry&)>& callback) {
        std::vector<NextTickListEntry> due;
        for (const auto& entry : ordered_) {
            if (entry.scheduled_time > now) break;
            if (static_cast<int>(due.size()) >= kMaxTick) break;
            due.push_back(entry);
        }
        for (const auto& entry : due) {
            const TargetKey target{entry.x, entry.y, entry.z, entry.block_id};
            if (const auto it = index_.find(target); it != index_.end()) {
                ordered_.erase(it->second);
                index_.erase(it);
            }
            callback(entry);
        }
    }

    [[nodiscard]] std::size_t size() const noexcept { return ordered_.size(); }
    [[nodiscard]] bool empty() const noexcept { return ordered_.empty(); }

private:
    struct TargetKey {
        int x;
        int y;
        int z;
        std::uint16_t block_id;

        [[nodiscard]] bool operator==(const TargetKey& other) const noexcept {
            return x == other.x && y == other.y && z == other.z && block_id == other.block_id;
        }
    };

    struct TargetKeyHash {
        [[nodiscard]] std::size_t operator()(const TargetKey& key) const noexcept {
            const auto hx = static_cast<std::size_t>(key.x) * 3129871U;
            const auto hz = static_cast<std::size_t>(key.z) * 116129781U;
            const auto hy = static_cast<std::size_t>(key.y) * 1013904223U;
            return (hx ^ hz ^ hy) ^ static_cast<std::size_t>(key.block_id);
        }
    };

    struct EntryLess {
        [[nodiscard]] bool operator()(const NextTickListEntry& lhs, const NextTickListEntry& rhs) const noexcept {
            if (lhs.scheduled_time != rhs.scheduled_time) return lhs.scheduled_time < rhs.scheduled_time;
            if (lhs.priority != rhs.priority) return lhs.priority < rhs.priority;
            return lhs.tick_entry_id < rhs.tick_entry_id;
        }
    };

    std::set<NextTickListEntry, EntryLess> ordered_;
    std::unordered_map<TargetKey, std::set<NextTickListEntry, EntryLess>::iterator, TargetKeyHash> index_;
    std::uint64_t next_id_{0};
};

}  // namespace cyane::world
