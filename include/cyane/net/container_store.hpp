#pragma once

#include <array>
#include <cstdint>
#include <mutex>
#include <unordered_map>

#include "cyane/item/item_stack.hpp"

namespace cyane::net {

// 世界中所有箱子容器的共享存储，按方块位置索引。多个 reactor 线程共享：
// 放置箱子时 ensure，打开/点击时读写同一份内容，故加锁。
class ContainerStore {
public:
    static constexpr std::size_t kChestSlots = 27;
    using Chest = std::array<item::ItemStack, kChestSlots>;

    // 确保 (bx,by,bz) 处存在一个空箱子容器（放置箱子时调用）
    void ensure(std::int64_t pos_key) {
        std::lock_guard<std::mutex> lock(mutex_);
        chests_.try_emplace(pos_key);
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

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::int64_t, Chest> chests_;
};

}
