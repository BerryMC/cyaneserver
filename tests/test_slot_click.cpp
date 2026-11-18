#include <array>
#include <cstdint>
#include <vector>

#include "cyane/net/slot_click.hpp"
#include "test_framework.hpp"

using cyane::item::ItemStack;
using cyane::net::DragState;
namespace click_type = cyane::net::click_type;
using cyane::net::drag_event_of;
using cyane::net::drag_mode_of;
using cyane::net::slot_click;
using cyane::item::kMaxStack;

namespace {

// 与 ContainerPlayer 一致的 45 槽布局：0..8 通用、9..35 主背包、36..44 热区栏
class FakeOps {
public:
    FakeOps() { slots.fill(ItemStack::air()); }

    [[nodiscard]] std::int16_t slot_count() const noexcept { return 45; }

    [[nodiscard]] ItemStack get(std::int16_t slot) const {
        return slots[static_cast<std::size_t>(slot)];
    }
    void set(std::int16_t slot, ItemStack stack) {
        slots[static_cast<std::size_t>(slot)] = stack;
    }
    [[nodiscard]] std::uint8_t stack_limit(std::int16_t, const ItemStack&) const noexcept {
        return kMaxStack;
    }
    // QUICK_MOVE：0..8 → 9..44（先叠加后填空位，余量写回源槽）
    [[nodiscard]] ItemStack transfer(std::int16_t slot) {
        auto moving = get(slot);
        for (std::int16_t i = 9; i < 45 && !moving.empty(); ++i) {
            auto dst = get(i);
            if (dst.empty()) {
                set(i, moving);
                moving = ItemStack::air();
                break;
            }
            if (dst.stacks_with(moving) && dst.count < kMaxStack) {
                const int total = static_cast<int>(dst.count) + static_cast<int>(moving.count);
                dst.count = static_cast<std::uint8_t>(std::min(total, 64));
                moving.count = static_cast<std::uint8_t>(total - dst.count);
                if (moving.count == 0) moving = ItemStack::air();
                set(i, dst);
            }
        }
        set(slot, moving);
        return moving;
    }
    void drop(ItemStack stack, bool whole) {
        dropped.push_back(stack);
        drop_whole.push_back(whole);
    }
    [[nodiscard]] bool add_to_inventory(ItemStack) { return true; }

    std::array<ItemStack, 45> slots;
    std::vector<ItemStack> dropped;
    std::vector<bool> drop_whole;
};

constexpr ItemStack make_stack(std::int16_t id, std::uint8_t count) {
    return ItemStack{id, count, 0};
}

}  // namespace

CYANE_TEST(slot_click_pickup_left_and_right) {
    FakeOps ops;
    DragState drag;
    ItemStack cursor{};
    ops.set(0, make_stack(1, 10));

    // 右键空光标 → 拿一半（向上取整）
    (void)slot_click(ops, cursor, drag, click_type::kPickup, 0, 1, false);
    CYANE_CHECK_EQ(static_cast<int>(cursor.count), 5);
    CYANE_CHECK_EQ(static_cast<int>(ops.get(0).count), 5);

    // 右键放到空槽 → 只放 1 个
    (void)slot_click(ops, cursor, drag, click_type::kPickup, 1, 1, false);
    CYANE_CHECK_EQ(static_cast<int>(ops.get(1).count), 1);
    CYANE_CHECK_EQ(static_cast<int>(cursor.count), 4);

    // 左键放回同类槽 → 合并
    (void)slot_click(ops, cursor, drag, click_type::kPickup, 1, 0, false);
    CYANE_CHECK_EQ(static_cast<int>(ops.get(1).count), 5);
    CYANE_CHECK(cursor.empty());
}

CYANE_TEST(slot_click_returns_pre_click_slot_content) {
    // 原版 slotClick 返回点击前的槽内容，ConfirmTransaction 靠它校验
    FakeOps ops;
    DragState drag;
    ItemStack cursor{};
    ops.set(2, make_stack(5, 7));
    const auto expected =
        slot_click(ops, cursor, drag, click_type::kPickup, 2, 0, false);
    CYANE_CHECK_EQ(static_cast<int>(expected.id), 5);
    CYANE_CHECK_EQ(static_cast<int>(expected.count), 7);
}

CYANE_TEST(slot_click_outside_window_drops_but_minus_one_does_nothing) {
    FakeOps ops;
    DragState drag;
    ItemStack cursor = make_stack(3, 12);

    // slotId = -1：无效槽，什么都不发生
    (void)slot_click(ops, cursor, drag, click_type::kPickup, -1, 0, false);
    CYANE_CHECK_EQ(static_cast<int>(cursor.count), 12);
    CYANE_CHECK(ops.dropped.empty());

    // slotId = -999 右键：只丢 1 个
    (void)slot_click(ops, cursor, drag, click_type::kPickup, -999, 1, false);
    CYANE_CHECK_EQ(static_cast<int>(cursor.count), 11);
    CYANE_CHECK_EQ(ops.dropped.size(), 1ULL);
    CYANE_CHECK_EQ(static_cast<int>(ops.dropped[0].count), 1);

    // slotId = -999 左键：整叠丢出并清空光标
    (void)slot_click(ops, cursor, drag, click_type::kPickup, -999, 0, false);
    CYANE_CHECK(cursor.empty());
    CYANE_CHECK_EQ(ops.dropped.size(), 2ULL);
    CYANE_CHECK_EQ(static_cast<int>(ops.dropped[1].count), 11);
}

CYANE_TEST(slot_click_swap_moves_hotbar_item) {
    FakeOps ops;
    DragState drag;
    ItemStack cursor{};
    ops.set(0, make_stack(10, 3));
    ops.set(36, make_stack(20, 5));  // 热区栏第一格（button 0 → 槽 36）

    // mode 2 / button 0 = 数字键 1 与槽 0 交换
    (void)slot_click(ops, cursor, drag, click_type::kSwap, 0, 0, false);
    CYANE_CHECK_EQ(static_cast<int>(ops.get(0).id), 20);
    CYANE_CHECK_EQ(static_cast<int>(ops.get(36).id), 10);
    CYANE_CHECK(cursor.empty());  // SWAP 不动光标
}

CYANE_TEST(slot_click_clone_only_in_creative) {
    FakeOps ops;
    DragState drag;
    ItemStack cursor{};
    ops.set(4, make_stack(42, 5));

    // 非创造模式：忽略
    (void)slot_click(ops, cursor, drag, click_type::kClone, 4, 2, false);
    CYANE_CHECK(cursor.empty());

    // 创造模式：整叠复制到光标，槽内容不变
    (void)slot_click(ops, cursor, drag, click_type::kClone, 4, 2, true);
    CYANE_CHECK_EQ(static_cast<int>(cursor.id), 42);
    CYANE_CHECK_EQ(static_cast<int>(cursor.count), 64);
    CYANE_CHECK_EQ(static_cast<int>(ops.get(4).count), 5);
}

CYANE_TEST(slot_click_throw_requires_empty_cursor) {
    FakeOps ops;
    DragState drag;
    ops.set(6, make_stack(7, 9));

    // 光标非空：Q 无效（原版 THROW 前置要求光标为空）
    ItemStack busy = make_stack(8, 2);
    (void)slot_click(ops, busy, drag, click_type::kThrow, 6, 0, false);
    CYANE_CHECK(ops.dropped.empty());
    CYANE_CHECK_EQ(static_cast<int>(ops.get(6).count), 9);

    // 光标为空：button 0 丢 1 个
    ItemStack cursor{};
    (void)slot_click(ops, cursor, drag, click_type::kThrow, 6, 0, false);
    CYANE_CHECK_EQ(static_cast<int>(ops.get(6).count), 8);
    CYANE_CHECK_EQ(ops.dropped.size(), 1ULL);

    // button 1：丢整叠
    (void)slot_click(ops, cursor, drag, click_type::kThrow, 6, 1, false);
    CYANE_CHECK(ops.get(6).empty());
}

CYANE_TEST(slot_click_quick_craft_even_split) {
    FakeOps ops;
    DragState drag;
    ItemStack cursor = make_stack(11, 9);

    // 开始拖拽（event 0），模式 0 = 均分
    (void)slot_click(ops, cursor, drag, click_type::kQuickCraft, -999, 0,
                                 false);
    CYANE_CHECK_EQ(drag.event, 1);  // 起始包把 event 置 1，结束包前不落物品
    CYANE_CHECK(drag.active());

    // 加两个槽
    (void)slot_click(ops, cursor, drag, click_type::kQuickCraft, 0, 1, false);
    (void)slot_click(ops, cursor, drag, click_type::kQuickCraft, 1, 1, false);

    // 结束（event 2）：每槽 9/2 = 4
    (void)slot_click(ops, cursor, drag, click_type::kQuickCraft, -999, 2,
                                 false);
    CYANE_CHECK(!drag.active());
    CYANE_CHECK_EQ(static_cast<int>(ops.get(0).count), 4);
    CYANE_CHECK_EQ(static_cast<int>(ops.get(1).count), 4);
    CYANE_CHECK_EQ(static_cast<int>(cursor.count), 1);
}

CYANE_TEST(slot_click_quick_craft_one_each) {
    FakeOps ops;
    DragState drag;
    ItemStack cursor = make_stack(12, 5);

    // 模式 1 = 每槽一个：button = (1 << 2) | event
    (void)slot_click(ops, cursor, drag, click_type::kQuickCraft, -999, 4,
                                 false);
    (void)slot_click(ops, cursor, drag, click_type::kQuickCraft, 0, 5, false);
    (void)slot_click(ops, cursor, drag, click_type::kQuickCraft, 1, 5, false);
    (void)slot_click(ops, cursor, drag, click_type::kQuickCraft, -999, 6,
                                 false);
    CYANE_CHECK_EQ(static_cast<int>(ops.get(0).count), 1);
    CYANE_CHECK_EQ(static_cast<int>(ops.get(1).count), 1);
    CYANE_CHECK_EQ(static_cast<int>(cursor.count), 3);
}

CYANE_TEST(slot_click_quick_move_transfers_and_keeps_leftover) {
    FakeOps ops;
    DragState drag;
    ItemStack cursor{};
    ops.set(0, make_stack(13, 20));
    ops.set(9, make_stack(13, 50));   // 只能再收 14
    for (std::int16_t i = 10; i < 45; ++i) {
        ops.set(i, make_stack(99, 64));  // 其余槽占满，余量无处可去
    }
    const auto expected =
        slot_click(ops, cursor, drag, click_type::kQuickMove, 0, 0, false);
    CYANE_CHECK_EQ(static_cast<int>(expected.count), 20);
    CYANE_CHECK_EQ(static_cast<int>(ops.get(9).count), 64);
    CYANE_CHECK_EQ(static_cast<int>(ops.get(0).count), 6);  // 余量留在源槽
}

CYANE_TEST(slot_click_drag_helpers_match_vanilla_bit_layout) {
    // Container.getDragEvent = button & 3；extractDragMode = button >> 2 & 3
    CYANE_CHECK_EQ(drag_event_of(0), 0);
    CYANE_CHECK_EQ(drag_event_of(1), 1);
    CYANE_CHECK_EQ(drag_event_of(2), 2);
    CYANE_CHECK_EQ(drag_event_of(6), 2);
    CYANE_CHECK_EQ(drag_mode_of(0), 0);
    CYANE_CHECK_EQ(drag_mode_of(4), 1);
    CYANE_CHECK_EQ(drag_mode_of(8), 2);
}
