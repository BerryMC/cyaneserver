#pragma once

#include <array>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

#include "cyane/item/item_stack.hpp"

namespace cyane::net {

// 世界中所有箱子容器的共享存储，按方块位置索引。多个 reactor 线程共享：
// 放置箱子时 ensure，打开/点击时读写同一份内容，故加锁。
class ContainerStore {
public:
    static constexpr std::size_t kChestSlots = 27;
    using Chest = std::array<item::ItemStack, kChestSlots>;

    // 小容器：发射器/投掷器 9 格，漏斗 5 格（统一 9 格数组，漏斗只用前 5）
    enum class SmallKind : std::uint8_t { dispenser = 0, dropper = 1, hopper = 2 };
    static constexpr std::size_t kSmallSlots = 9;
    static constexpr std::size_t kHopperSlots = 5;
    struct SmallContainer {
        SmallKind kind{SmallKind::dispenser};
        std::array<item::ItemStack, kSmallSlots> slots{};
    };

    // 确保 (bx,by,bz) 处存在一个空箱子容器（放置箱子时调用）
    void ensure(std::int64_t pos_key) {
        std::lock_guard<std::mutex> lock(mutex_);
        chests_.try_emplace(pos_key);
    }

    // ---- 小容器（发射器/投掷器/漏斗）----

    void ensure_small(std::int64_t pos_key, SmallKind kind) {
        std::lock_guard<std::mutex> lock(mutex_);
        small_.try_emplace(pos_key, SmallContainer{kind, {}});
    }

    void remove_small(std::int64_t pos_key) {
        std::lock_guard<std::mutex> lock(mutex_);
        small_.erase(pos_key);
    }

    [[nodiscard]] bool small_exists(std::int64_t pos_key) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return small_.contains(pos_key);
    }

    [[nodiscard]] SmallContainer snapshot_small(std::int64_t pos_key) const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (auto it = small_.find(pos_key); it != small_.end()) {
            return it->second;
        }
        return SmallContainer{};
    }

    [[nodiscard]] item::ItemStack small_slot(std::int64_t pos_key, std::size_t index) const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (auto it = small_.find(pos_key); it != small_.end() && index < kSmallSlots) {
            return it->second.slots[index];
        }
        return item::ItemStack::air();
    }

    void set_small_slot(std::int64_t pos_key, std::size_t index, item::ItemStack item) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = small_.find(pos_key);
        if (it == small_.end() || index >= kSmallSlots) {
            return;
        }
        if (it->second.kind == SmallKind::hopper && index >= kHopperSlots) {
            return;  // 漏斗只有 5 格
        }
        it->second.slots[index] = item;
    }

    // 全量快照（存档：按区块归组写 TileEntities）
    [[nodiscard]] std::vector<std::pair<std::int64_t, SmallContainer>> all_small() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return {small_.begin(), small_.end()};
    }

    void remove(std::int64_t pos_key) {
        std::lock_guard<std::mutex> lock(mutex_);
        chests_.erase(pos_key);
    }

    [[nodiscard]] bool exists(std::int64_t pos_key) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return chests_.contains(pos_key);
    }

    // 拷贝出某箱子的全部内容（打开窗口时下发）
    [[nodiscard]] Chest snapshot(std::int64_t pos_key) const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (auto it = chests_.find(pos_key); it != chests_.end()) {
            return it->second;
        }
        return Chest{};
    }

    [[nodiscard]] item::ItemStack slot(std::int64_t pos_key, std::size_t index) const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (auto it = chests_.find(pos_key); it != chests_.end() && index < kChestSlots) {
            return it->second[index];
        }
        return item::ItemStack::air();
    }

    void set_slot(std::int64_t pos_key, std::size_t index, item::ItemStack item) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (auto it = chests_.find(pos_key); it != chests_.end() && index < kChestSlots) {
            it->second[index] = item;
        }
    }

    // 全量快照（存档：按区块归组写 TileEntities）
    [[nodiscard]] std::vector<std::pair<std::int64_t, Chest>> all() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return {chests_.begin(), chests_.end()};
    }

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::int64_t, Chest> chests_;
    std::unordered_map<std::int64_t, SmallContainer> small_;
};

}
