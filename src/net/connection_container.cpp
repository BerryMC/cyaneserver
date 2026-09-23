#include "cyane/net/connection.hpp"

#include <algorithm>

#include "connection_detail.hpp"
#include "cyane/core/log.hpp"
#include "cyane/proto/json.hpp"

namespace cyane::net {

namespace {
// 单箱窗口布局：0..26 箱子槽，27..53 主背包(玩家 9..35)，54..62 热区栏(玩家 36..44)
constexpr std::int16_t kChestSlots = 27;
constexpr std::int16_t kChestWindowSlots = 63;

// 窗口槽 → 玩家背包槽（仅当 >=27 时有效）
[[nodiscard]] std::size_t window_to_player_slot(std::int16_t win_slot) noexcept {
    if (win_slot < 27 + 27) {
        return 9 + static_cast<std::size_t>(win_slot - 27);  // 主背包
    }
    return 36 + static_cast<std::size_t>(win_slot - 54);  // 热区栏
}
}

void Connection::open_chest(std::int64_t chest_key) {
    if (context_.containers == nullptr) {
        return;
    }
    open_chest_key_ = chest_key;
    chest_open_ = true;

    // OpenWindow (0x13)：byte windowId | string windowType | chat title | ubyte slotCount
    ByteWriter open;
    open.u8(kChestWindowId);
    open.string("minecraft:chest");
    open.string(proto::chat_text("Chest"));
    open.u8(static_cast<std::uint8_t>(kChestSlots));
    send_packet(proto::play_cb::kOpenWindow, open.data());

    // WindowItems (0x14)：整窗 63 槽（箱子 27 + 主背包 27 + 热区栏 9）
    ByteWriter items;
    items.u8(kChestWindowId);
    items.i16(kChestWindowSlots);
    const auto chest = context_.containers->snapshot(chest_key);
    for (std::int16_t i = 0; i < kChestSlots; ++i) {
        item::write_slot(items, chest[static_cast<std::size_t>(i)]);
    }
    for (std::int16_t i = kChestSlots; i < kChestWindowSlots; ++i) {
        item::write_slot(items, inventory_.slot(window_to_player_slot(i)));
    }
    send_packet(proto::play_cb::kWindowItems, items.data());
}

bool Connection::handle_play_close_window(ByteSpan payload) {
    // CloseWindow (sb 0x08)：byte windowId。客户端关闭箱子窗口时发。
    (void)payload;
    chest_open_ = false;
    open_chest_key_ = 0;
    // 关窗时游标物品退回背包，放不下则丢弃（本阶段不回吐掉落物）
    if (!cursor_item_.empty()) {
        cursor_item_ = give_item(cursor_item_);
        cursor_item_ = item::ItemStack::air();
    }
    return true;
}

void Connection::apply_chest_click(std::int16_t slot, std::uint8_t button, std::int32_t mode) {
    if (context_.containers == nullptr || !chest_open_) {
        return;
    }
    (void)mode;  // 箱子窗口本阶段仅支持普通左/右键，不做 shift 转移
    if (slot < 0 || slot >= kChestWindowSlots) {
        // 窗口外：丢弃游标
        if (slot < 0) {
            cursor_item_ = item::ItemStack::air();
        }
        return;
    }
    const bool is_chest = slot < kChestSlots;
    // 读取被点槽当前物品
    item::ItemStack in_slot = is_chest
        ? context_.containers->slot(open_chest_key_, static_cast<std::size_t>(slot))
        : inventory_.slot(window_to_player_slot(slot));

    if (button == 0) {
        // 左键：合并同类或整堆交换
        if (cursor_item_.stacks_with(in_slot)) {
            const int total = static_cast<int>(in_slot.count) + static_cast<int>(cursor_item_.count);
            const int keep = std::min(total, static_cast<int>(item::kMaxStack));
            in_slot.count = static_cast<std::uint8_t>(keep);
            cursor_item_.count = static_cast<std::uint8_t>(total - keep);
            if (cursor_item_.count == 0) {
                cursor_item_ = item::ItemStack::air();
            }
        } else {
            std::swap(cursor_item_, in_slot);
        }
    } else if (button == 1) {
        // 右键：放一个 / 拿一半
        if (cursor_item_.empty()) {
            if (!in_slot.empty()) {
                const std::uint8_t take = static_cast<std::uint8_t>((in_slot.count + 1) / 2);
                cursor_item_ = in_slot;
                cursor_item_.count = take;
                in_slot.count = static_cast<std::uint8_t>(in_slot.count - take);
                if (in_slot.count == 0) {
                    in_slot = item::ItemStack::air();
                }
            }
        } else if (in_slot.empty()) {
            in_slot = cursor_item_;
            in_slot.count = 1;
            --cursor_item_.count;
            if (cursor_item_.count == 0) {
                cursor_item_ = item::ItemStack::air();
            }
        } else if (cursor_item_.stacks_with(in_slot) && in_slot.count < item::kMaxStack) {
            ++in_slot.count;
            --cursor_item_.count;
            if (cursor_item_.count == 0) {
                cursor_item_ = item::ItemStack::air();
            }
        } else {
            std::swap(cursor_item_, in_slot);
        }
    } else {
        return;
    }

    // 写回权威状态并回发被点槽 + 游标
    if (is_chest) {
        context_.containers->set_slot(open_chest_key_, static_cast<std::size_t>(slot), in_slot);
    } else {
        inventory_.set_slot(window_to_player_slot(slot), in_slot);
    }
    send_slot(static_cast<std::int8_t>(kChestWindowId), slot, in_slot);
    send_slot(-1, -1, cursor_item_);
}

}
