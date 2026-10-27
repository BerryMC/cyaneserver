#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "cyane/item/item_stack.hpp"
#include "cyane/world/loot/loot_table.hpp"
#include "cyane/world/noise.hpp"

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

    // ---- 战利品箱（vanilla TileEntityChest 的 LootTable + LootTableSeed）----
    // 世界生成时标记某位置的箱子带掉落表；首次访问时按种子生成一次内容并清除标记。
    void set_loot_manager(std::shared_ptr<world::loot::LootTableManager> manager) {
        std::lock_guard<std::mutex> lock(mutex_);
        loot_manager_ = std::move(manager);
    }

    void mark_loot(std::int64_t pos_key, std::string table, std::int64_t seed) {
        std::lock_guard<std::mutex> lock(mutex_);
        chests_.try_emplace(pos_key);
        loot_marks_[pos_key] = LootMark{std::move(table), seed};
    }

    [[nodiscard]] bool has_loot(std::int64_t pos_key) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return loot_marks_.contains(pos_key);
    }

    // vanilla TileEntityChest.fillWithLoot：按掉落表生成后随机塞入空槽位，随后可重复读写
    void fill_loot_locked(std::int64_t pos_key) const {
        auto& marks = const_cast<std::unordered_map<std::int64_t, LootMark>&>(loot_marks_);
        const auto mark_it = marks.find(pos_key);
        if (mark_it == marks.end() || loot_manager_ == nullptr) return;
        const auto mark = mark_it->second;
        marks.erase(mark_it);

        world::gen::JavaRandom rand{static_cast<std::uint64_t>(mark.seed)};
        auto stacks = loot_manager_->generate(mark.table, rand, world::loot::LootContext{});
        auto& chests = const_cast<std::unordered_map<std::int64_t, Chest>&>(chests_);
        auto& chest = chests[pos_key];
        for (const auto& stack : stacks) {
            // vanilla：随机挑槽位，连续 3 次撞到非空即丢弃该件
            for (int attempt = 0; attempt < 3; ++attempt) {
                const auto index = static_cast<std::size_t>(rand.next_int(static_cast<int>(kChestSlots)));
                if (chest[index].empty()) {
                    chest[index] = stack;
                    break;
                }
            }
        }
    }

    // 拷贝出某箱子的全部内容（打开窗口时下发）
    [[nodiscard]] Chest snapshot(std::int64_t pos_key) const {
        std::lock_guard<std::mutex> lock(mutex_);
        fill_loot_locked(pos_key);
        if (auto it = chests_.find(pos_key); it != chests_.end()) {
            return it->second;
        }
        return Chest{};
    }

    [[nodiscard]] item::ItemStack slot(std::int64_t pos_key, std::size_t index) const {
        std::lock_guard<std::mutex> lock(mutex_);
        fill_loot_locked(pos_key);
        if (auto it = chests_.find(pos_key); it != chests_.end() && index < kChestSlots) {
            return it->second[index];
        }
        return item::ItemStack::air();
    }

    void set_slot(std::int64_t pos_key, std::size_t index, item::ItemStack item) {
        std::lock_guard<std::mutex> lock(mutex_);
        fill_loot_locked(pos_key);
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
    struct LootMark {
        std::string table;
        std::int64_t seed{0};
    };

    mutable std::mutex mutex_;
    std::unordered_map<std::int64_t, Chest> chests_;
    std::unordered_map<std::int64_t, LootMark> loot_marks_;
    std::shared_ptr<world::loot::LootTableManager> loot_manager_;
    std::unordered_map<std::int64_t, SmallContainer> small_;
};

}
