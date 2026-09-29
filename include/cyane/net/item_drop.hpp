#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <span>
#include <vector>

#include "cyane/entity/player_manager.hpp"
#include "cyane/item/item_stack.hpp"

namespace cyane::net {

// 世界中的一个掉落物实体：位置 + 物品堆叠 + 出生时刻（用于拾取延迟）。
struct DroppedItem {
    std::uint32_t entity_id{0};
    double x{0.0};
    double y{0.0};
    double z{0.0};
    item::ItemStack stack;
    std::uint64_t spawn_ms{0};
};

// 持久化形状：位置 + 堆叠（存档 Entities 列表的最小字段集）
struct DroppedItemState {
    double x{0.0};
    double y{0.0};
    double z{0.0};
    item::ItemStack stack;
};

// 一次被某玩家拾取的结果：谁拾取、拾取了哪个实体、堆叠内容。
struct PickupEvent {
    std::uint32_t item_entity_id{0};
    std::uint32_t collector_id{0};
    item::ItemStack stack;
    double x{0.0};
    double y{0.0};
    double z{0.0};
};

// 线程安全的掉落物登记表。多个 reactor 线程共享：破坏方块时 spawn，
// 每次连接 tick 时 collect_near 检测拾取。
class ItemDropManager {
public:
    // 生成一个掉落物，返回其实体 id（0 表示空堆叠未生成）
    std::uint32_t spawn(double x, double y, double z, item::ItemStack stack, std::uint64_t now_ms) {
        if (stack.empty()) {
            return 0;
        }
        const std::uint32_t id = entity::allocate_entity_id();
        std::lock_guard<std::mutex> lock(mutex_);
        items_.push_back(DroppedItem{id, x, y, z, stack, now_ms});
        return id;
    }

    // 从存档恢复掉落物（spawn_ms=0，过延迟即拾取）；空堆叠忽略
    void restore(std::span<const DroppedItemState> drops) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& drop : drops) {
            if (drop.stack.empty()) {
                continue;
            }
            items_.push_back(DroppedItem{entity::allocate_entity_id(), drop.x, drop.y, drop.z,
                                        drop.stack, 0});
        }
    }

    // 全量快照（保存存档用：位置 + 堆叠）
    [[nodiscard]] std::vector<DroppedItemState> all_drops() const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<DroppedItemState> out;
        out.reserve(items_.size());
        for (const auto& it : items_) {
            out.push_back(DroppedItemState{it.x, it.y, it.z, it.stack});
        }
        return out;
    }

    // 当前所有掉落物快照（新玩家进入时补发）
    [[nodiscard]] std::vector<DroppedItem> snapshot() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return items_;
    }

    // 检测 collector 在 (px,py,pz) 半径内、且已过拾取延迟的掉落物，原子取走并返回。
    [[nodiscard]] std::vector<PickupEvent> collect_near(std::uint32_t collector, double px, double py,
                                                        double pz, std::uint64_t now_ms) {
        std::vector<PickupEvent> picked;
        std::lock_guard<std::mutex> lock(mutex_);
        for (std::size_t i = 0; i < items_.size();) {
            const DroppedItem& it = items_[i];
            const double dx = it.x - px;
            const double dy = it.y - py;
            const double dz = it.z - pz;
            const bool ready = now_ms - it.spawn_ms >= kPickupDelayMs;
            if (ready && dx * dx + dy * dy + dz * dz <= kPickupRadiusSq) {
                picked.push_back(PickupEvent{it.entity_id, collector, it.stack, it.x, it.y, it.z});
                items_[i] = items_.back();
                items_.pop_back();
            } else {
                ++i;
            }
        }
        return picked;
    }

private:
    static constexpr std::uint64_t kPickupDelayMs = 500;  // 出生后短暂不可拾取，避免立即回吸
    static constexpr double kPickupRadiusSq = 1.5 * 1.5;

    mutable std::mutex mutex_;
    std::vector<DroppedItem> items_;
};

}
