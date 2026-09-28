#pragma once

#include <array>
#include <cstdint>
#include <mutex>
#include <unordered_map>

#include "cyane/item/item_stack.hpp"

namespace cyane::world {

// 工作台（方块 id 58）3x3 格状态：9 格物品按方块位置持久化（同箱子），
// 合成结果随格内容即时计算、不存储
class CraftingTableStore {
public:
    static constexpr std::size_t kGridCells = 9;

    void ensure(std::int64_t key) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        cells_.try_emplace(key);
    }

    void remove(std::int64_t key) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        cells_.erase(key);
    }

    [[nodiscard]] bool exists(std::int64_t key) const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return cells_.contains(key);
    }

    [[nodiscard]] std::array<item::ItemStack, kGridCells> cells(std::int64_t key) const {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = cells_.find(key);
        if (it == cells_.end()) {
            return {};
        }
        return it->second;
    }

    void set_cell(std::int64_t key, std::size_t index, item::ItemStack stack) noexcept {
        if (index >= kGridCells) {
            return;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        auto& cell = cells_[key];
        cell[index] = std::move(stack);
    }

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::int64_t, std::array<item::ItemStack, kGridCells>> cells_;
};

} // namespace cyane::world
