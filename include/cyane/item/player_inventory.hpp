#pragma once

#include <array>
#include <cstdint>

#include "cyane/item/item_stack.hpp"

namespace cyane::item {

// 1.12.2 玩家窗口(windowId=0)槽位布局，共 46 槽：
//   0        合成结果
//   1..4     2x2 合成格
//   5..8     护甲（头/胸/腿/脚）
//   9..35    主背包 3 行 27 格
//   36..44   热区栏 9 格
//   45       副手
class PlayerInventory {
public:
    static constexpr std::size_t kSlotCount = 46;
    static constexpr std::size_t kHotbarStart = 36;
    static constexpr std::size_t kHotbarCount = 9;

    [[nodiscard]] const ItemStack& slot(std::size_t index) const noexcept {
        return index < kSlotCount ? slots_[index] : empty_;
    }

    void set_slot(std::size_t index, ItemStack item) noexcept {
        if (index < kSlotCount) {
            slots_[index] = item;
        }
    }

    // 热区栏索引(0..8) → 窗口槽位
    [[nodiscard]] static std::size_t hotbar_slot(std::uint8_t hotbar) noexcept {
        return kHotbarStart + (hotbar % kHotbarCount);
    }

    [[nodiscard]] const ItemStack& hotbar_item(std::uint8_t hotbar) const noexcept {
        return slot(hotbar_slot(hotbar));
    }

    [[nodiscard]] std::array<ItemStack, kSlotCount>& slots() noexcept { return slots_; }
    [[nodiscard]] const std::array<ItemStack, kSlotCount>& slots() const noexcept { return slots_; }

private:
    std::array<ItemStack, kSlotCount> slots_{};
    static constexpr ItemStack empty_{};
};

}
