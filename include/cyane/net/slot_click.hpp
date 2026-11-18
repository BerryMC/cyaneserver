#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

#include "cyane/item/item_stack.hpp"

namespace cyane::net {

// PacketPlayInClickWindow 的 ClickType 序数（EnumClickType 声明顺序）
namespace click_type {
inline constexpr std::int32_t kPickup = 0;
inline constexpr std::int32_t kQuickMove = 1;
inline constexpr std::int32_t kSwap = 2;
inline constexpr std::int32_t kClone = 3;
inline constexpr std::int32_t kThrow = 4;
inline constexpr std::int32_t kQuickCraft = 5;
inline constexpr std::int32_t kPickupAll = 6;
}

// Container.getDragEvent：button 低 2 位 = 0 开始 / 1 加槽 / 2 结束
[[nodiscard]] constexpr std::int32_t drag_event_of(std::int8_t button) noexcept {
    return static_cast<std::int32_t>(button) & 3;
}
// Container.extractDragMode：button 高 2 位 = 0 均分 / 1 每槽一个 / 2 创造模式铺满
[[nodiscard]] constexpr std::int32_t drag_mode_of(std::int8_t button) noexcept {
    return (static_cast<std::int32_t>(button) >> 2) & 3;
}

// Container.dragEvent / dragMode / dragSlots 的等价状态
struct DragState {
    std::int32_t event{0};
    std::int32_t mode{0};
    std::vector<std::int16_t> slots;

    void reset() noexcept {
        event = 0;
        mode = 0;
        slots.clear();
    }
    [[nodiscard]] bool active() const noexcept { return event != 0; }
};

// 窗口槽的可移植实现：Ops 需提供
//   std::int16_t slot_count() const
//   item::ItemStack get(std::int16_t slot) const
//   void set(std::int16_t slot, item::ItemStack stack)
//   item::ItemStack transfer(std::int16_t slot)          // QUICK_MOVE，返回未搬走的余量
//   void drop(item::ItemStack stack, bool whole_stack)   // 丢弃到世界
// 槽的堆叠上限默认 64（项目尚无逐物品 maxStack 表）。
//
// 返回值 = 原版 slotClick 的返回栈，供 ConfirmTransaction 与客户端 clickedItem 比对。
template <typename Ops>
[[nodiscard]] item::ItemStack slot_click(Ops& ops, item::ItemStack& cursor, DragState& drag,
                                        std::int32_t mode, std::int16_t slot, std::int8_t button,
                                        bool creative) {
    using item::ItemStack;
    constexpr std::uint8_t kMax = item::kMaxStack;
    const auto max_of = [&](std::int16_t s, const ItemStack& stack) -> std::uint8_t {
        return std::min<std::uint8_t>(kMax, ops.stack_limit(s, stack));
    };
    // canAddItemToSlot(slot, stack, /*stackSizeMatters=*/true)
    const auto can_add = [&](std::int16_t s, const ItemStack& stack) {
        const auto in = ops.get(s);
        if (in.empty()) return true;
        return in.stacks_with(stack) && in.count <= kMax;
    };
    const auto grow = [](ItemStack& stack, std::uint8_t amount) {
        const int total = static_cast<int>(stack.count) + static_cast<int>(amount);
        stack.count = static_cast<std::uint8_t>(std::min(total, static_cast<int>(kMax)));
    };

    ItemStack expected;  // 默认空（SWAP/CLONE/THROW/PICKUP_ALL 都不回填）

    // ---- QUICK_CRAFT（拖拽）----
    if (mode == click_type::kQuickCraft) {
        const std::int32_t previous = drag.event;
        drag.event = drag_event_of(button);
        if ((previous != 1 || drag.event != 2) && previous != drag.event) {
            drag.reset();
        } else if (cursor.empty()) {
            drag.reset();
        } else if (drag.event == 0) {
            drag.mode = drag_mode_of(button);
            const bool valid = drag.mode == 0 || drag.mode == 1 || (drag.mode == 2 && creative);
            if (valid) {
                drag.event = 1;
                drag.slots.clear();
            } else {
                drag.reset();
            }
        } else if (drag.event == 1) {
            if (slot >= 0 && slot < ops.slot_count() && can_add(slot, cursor) &&
                (drag.mode == 2 || cursor.count > drag.slots.size())) {
                if (std::find(drag.slots.begin(), drag.slots.end(), slot) == drag.slots.end()) {
                    drag.slots.push_back(slot);
                }
            }
        } else if (drag.event == 2) {
            if (!drag.slots.empty()) {
                const ItemStack template_stack = cursor;
                int remaining = static_cast<int>(cursor.count);
                for (const auto target : drag.slots) {
                    if (target < 0 || target >= ops.slot_count()) continue;
                    if (!can_add(target, cursor)) continue;
                    if (!(drag.mode == 2 || static_cast<int>(cursor.count) >=
                                              static_cast<int>(drag.slots.size()))) {
                        continue;
                    }
                    ItemStack place = template_stack;
                    const auto existing = ops.get(target);
                    const int slot_stack_size = existing.empty() ? 0 : static_cast<int>(existing.count);
                    switch (drag.mode) {
                        case 0:
                            place.count = static_cast<std::uint8_t>(
                                static_cast<int>(place.count) /
                                std::max<int>(1, static_cast<int>(drag.slots.size())));
                            break;
                        case 1:
                            place.count = 1;
                            break;
                        default:
                            place.count = kMax;
                            break;
                    }
                    grow(place, static_cast<std::uint8_t>(slot_stack_size));
                    const auto limit = max_of(target, place);
                    if (place.count > limit) {
                        place.count = limit;
                    }
                    remaining -= static_cast<int>(place.count) - slot_stack_size;
                    ops.set(target, place);
                }
                ItemStack left = template_stack;
                left.count = static_cast<std::uint8_t>(std::max(0, remaining));
                cursor = left.empty() ? ItemStack::air() : left;
            }
            drag.reset();
        } else {
            drag.reset();
        }
        return expected;
    }
    if (drag.active()) {
        drag.reset();
    }

    // ---- PICKUP / QUICK_MOVE ----
    if ((mode == click_type::kPickup || mode == click_type::kQuickMove) &&
        (button == 0 || button == 1)) {
        if (slot == -999) {
            if (!cursor.empty()) {
                if (button == 0) {
                    ops.drop(cursor, true);
                    cursor = ItemStack::air();
                } else {
                    ItemStack one = cursor;
                    one.count = 1;
                    cursor.count = static_cast<std::uint8_t>(cursor.count - 1);
                    if (cursor.count == 0) cursor = ItemStack::air();
                    ops.drop(one, false);
                }
            }
            return expected;
        }
        if (mode == click_type::kQuickMove) {
            if (slot < 0 || slot >= ops.slot_count()) return expected;
            // 原版循环：反复 transfer 直到余量不再变化
            for (int guard = 0; guard < 8; ++guard) {
                const auto before = ops.get(slot);
                if (before.empty()) break;
                const auto leftover = ops.transfer(slot);
                if (leftover.empty()) {
                    expected = before;
                    break;
                }
                if (leftover.count == before.count) break;  // 搬不动了
                expected = before;
            }
            return expected;
        }
        // PICKUP 单槽
        if (slot < 0 || slot >= ops.slot_count()) return expected;
        ItemStack in_slot = ops.get(slot);
        if (!in_slot.empty()) expected = in_slot;

        if (in_slot.empty()) {
            if (!cursor.empty()) {
                const std::uint8_t put = std::min<std::uint8_t>(
                    button == 0 ? cursor.count : 1, max_of(slot, cursor));
                ItemStack placed = cursor;
                placed.count = put;
                cursor.count = static_cast<std::uint8_t>(cursor.count - put);
                if (cursor.count == 0) cursor = ItemStack::air();
                ops.set(slot, placed);
            }
        } else if (cursor.empty()) {
            const std::uint8_t take =
                button == 0 ? in_slot.count
                            : static_cast<std::uint8_t>((in_slot.count + 1) / 2);
            ItemStack taken = in_slot;
            taken.count = take;
            in_slot.count = static_cast<std::uint8_t>(in_slot.count - take);
            cursor = taken;
            ops.set(slot, in_slot.count == 0 ? ItemStack::air() : in_slot);
        } else if (in_slot.stacks_with(cursor)) {
            std::uint8_t move = button == 0 ? cursor.count : 1;
            const auto limit = max_of(slot, cursor);
            move = std::min<std::uint8_t>(move, static_cast<std::uint8_t>(limit - in_slot.count));
            move = std::min<std::uint8_t>(move, static_cast<std::uint8_t>(kMax - in_slot.count));
            in_slot.count = static_cast<std::uint8_t>(in_slot.count + move);
            cursor.count = static_cast<std::uint8_t>(cursor.count - move);
            if (cursor.count == 0) cursor = ItemStack::air();
            ops.set(slot, in_slot);
        } else if (cursor.count <= max_of(slot, cursor)) {
            std::swap(cursor, in_slot);
            ops.set(slot, in_slot);
        }
        return expected;
    }

    // ---- SWAP（数字键换位）----
    if (mode == click_type::kSwap && button >= 0 && button < 9) {
        if (slot < 0 || slot >= ops.slot_count()) return expected;
        // button 是热区栏序号，映射到玩家背包槽 36..44
        const std::int16_t hotbar = static_cast<std::int16_t>(36 + button);
        if (hotbar >= ops.slot_count()) return expected;
        ItemStack hot = ops.get(hotbar);
        ItemStack in_slot = ops.get(slot);
        if (!hot.empty() || !in_slot.empty()) {
            if (hot.empty()) {
                ops.set(hotbar, in_slot);
                ops.set(slot, ItemStack::air());
            } else if (in_slot.empty()) {
                const auto limit = max_of(slot, hot);
                if (hot.count > limit) {
                    ItemStack placed = hot;
                    placed.count = limit;
                    hot.count = static_cast<std::uint8_t>(hot.count - limit);
                    ops.set(slot, placed);
                } else {
                    ops.set(slot, hot);
                    ops.set(hotbar, ItemStack::air());
                }
            } else {
                const auto limit = max_of(slot, hot);
                if (hot.count > limit) {
                    ItemStack placed = hot;
                    placed.count = limit;
                    hot.count = static_cast<std::uint8_t>(hot.count - limit);
                    ops.set(slot, placed);
                    // 原版把原槽内容塞回背包，失败则丢出
                    if (!ops.add_to_inventory(in_slot)) ops.drop(in_slot, true);
                } else {
                    ops.set(slot, hot);
                    ops.set(hotbar, in_slot);
                }
            }
        }
        return expected;
    }

    // ---- CLONE（创造模式中键复制）----
    if (mode == click_type::kClone) {
        if (!creative || !cursor.empty() || slot < 0 || slot >= ops.slot_count()) {
            return expected;
        }
        const auto in_slot = ops.get(slot);
        if (!in_slot.empty()) {
            cursor = in_slot;
            cursor.count = max_of(slot, in_slot);
        }
        return expected;
    }

    // ---- THROW（Q 丢弃）----
    if (mode == click_type::kThrow) {
        if (!cursor.empty() || slot < 0 || slot >= ops.slot_count()) return expected;
        auto in_slot = ops.get(slot);
        if (!in_slot.empty()) {
            const std::uint8_t amount = button == 0 ? 1 : in_slot.count;
            ItemStack thrown = in_slot;
            thrown.count = amount;
            in_slot.count = static_cast<std::uint8_t>(in_slot.count - amount);
            ops.set(slot, in_slot.count == 0 ? ItemStack::air() : in_slot);
            ops.drop(thrown, true);
        }
        return expected;
    }

    // ---- PICKUP_ALL（双击收集）----
    if (mode == click_type::kPickupAll) {
        if (slot < 0 || slot >= ops.slot_count() || cursor.empty()) return expected;
        const bool forward = button == 0;
        for (int pass = 0; pass < 2 && cursor.count < kMax; ++pass) {
            if (forward) {
                for (std::int16_t i = 0; i < ops.slot_count() && cursor.count < kMax; ++i) {
                    if (pass == 0 && i == slot) continue;
                    auto other = ops.get(i);
                    if (other.empty() || !other.stacks_with(cursor) || !can_add(i, cursor)) continue;
                    if (pass == 0 && other.count == kMax) continue;
                    const std::uint8_t move = std::min<std::uint8_t>(
                        static_cast<std::uint8_t>(kMax - cursor.count), other.count);
                    other.count = static_cast<std::uint8_t>(other.count - move);
                    cursor.count = static_cast<std::uint8_t>(cursor.count + move);
                    ops.set(i, other.count == 0 ? ItemStack::air() : other);
                }
            } else {
                for (std::int16_t i = static_cast<std::int16_t>(ops.slot_count() - 1);
                     i >= 0 && cursor.count < kMax; --i) {
                    if (pass == 0 && i == slot) continue;
                    auto other = ops.get(i);
                    if (other.empty() || !other.stacks_with(cursor) || !can_add(i, cursor)) continue;
                    if (pass == 0 && other.count == kMax) continue;
                    const std::uint8_t move = std::min<std::uint8_t>(
                        static_cast<std::uint8_t>(kMax - cursor.count), other.count);
                    other.count = static_cast<std::uint8_t>(other.count - move);
                    cursor.count = static_cast<std::uint8_t>(cursor.count + move);
                    ops.set(i, other.count == 0 ? ItemStack::air() : other);
                }
            }
        }
        return expected;
    }

    return expected;
}

}  // namespace cyane::net
