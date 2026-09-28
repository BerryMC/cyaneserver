#include "cyane/net/connection.hpp"

#include <algorithm>

#include "connection_detail.hpp"
#include "cyane/core/log.hpp"
#include "cyane/proto/json.hpp"

namespace cyane::net {

namespace {
// 单箱窗口布局：0..26 箱子槽，27..53 主背包(玩家 9..35)，54..62 热区栏(玩家 36..44)
using detail::kChestSlots;
using detail::kChestWindowSlots;

// 窗口槽 → 玩家背包槽（仅当 >=27 时有效）
[[nodiscard]] std::size_t window_to_player_slot(std::int16_t win_slot) noexcept {
    return detail::container_window_to_player_slot(win_slot, kChestSlots);
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

void Connection::close_client_window(std::uint8_t window_id) {
    ByteWriter out;
    out.u8(window_id);
    send_packet(proto::play_cb::kCloseWindow, out.data());
    // 关窗前游标上的物品退回背包，放不下则落地
    if (!cursor_item_.empty()) {
        const item::ItemStack leftover = give_item(cursor_item_);
        if (!leftover.empty()) {
            const auto cpos = world::ChunkPos::from_world(static_cast<std::int32_t>(player_pos_.x),
                                                           static_cast<std::int32_t>(player_pos_.z));
            drop_stack(player_pos_.x, player_pos_.y, player_pos_.z, leftover,
                       cpos ? cpos->x : 0, cpos ? cpos->z : 0);
        }
        cursor_item_ = item::ItemStack::air();
    }
}

bool Connection::handle_play_close_window(ByteSpan payload) {
    (void)payload;
    // CloseWindow (sb 0x08)：byte windowId。客户端关闭箱子/熔炉/工作台窗口时发。
    if (furnace_open_ && context_.furnaces != nullptr && open_furnace_key_ != 0) {
        // 关闭熔炉窗口前，确保炉灶状态已同步到存储（FurnaceStore 已在点击时即时更新）
        // 此处仅重置连接状态，不清除 FurnaceStore 数据
    }
    chest_open_ = false;
    open_chest_key_ = 0;
    furnace_open_ = false;
    open_furnace_key_ = 0;
    table_open_ = false;
    open_table_key_ = 0;
    // 关窗时游标物品退回背包，放不下的部分生成掉落物（避免物品凭空消失）
    if (!cursor_item_.empty()) {
        const item::ItemStack leftover = give_item(cursor_item_);
        if (!leftover.empty()) {
            const auto cpos = world::ChunkPos::from_world(static_cast<std::int32_t>(player_pos_.x),
                                                           static_cast<std::int32_t>(player_pos_.z));
            drop_stack(player_pos_.x, player_pos_.y, player_pos_.z, leftover,
                       cpos ? cpos->x : 0, cpos ? cpos->z : 0);
        }
        cursor_item_ = item::ItemStack::air();
    }
    return true;
}

void Connection::apply_chest_click(std::int16_t slot, std::uint8_t button, std::int32_t mode,
                                   const item::ItemStack& clicked) {
    if (context_.containers == nullptr || !chest_open_) {
        return;
    }
    // 创造模式：客户端经 clickedItem 声明创造选择器取出的物品；服务端游标为空时采信
    if (context_.game_mode == proto::game_mode::kCreative && cursor_item_.empty() && !clicked.empty()) {
        cursor_item_ = clicked;
    }
    if (slot < 0 || slot >= kChestWindowSlots) {
        // 窗口外：丢弃游标
        if (slot < 0) {
            cursor_item_ = item::ItemStack::air();
        }
        return;
    }
    const bool is_chest = slot < kChestSlots;

    if (mode == 1) {
        // shift 快速转移：箱子槽 → 主背包再热区栏（余量留原槽）；玩家槽 → 热区栏↔主背包
        if (is_chest) {
            auto moving = context_.containers->slot(open_chest_key_, static_cast<std::size_t>(slot));
            if (moving.empty()) {
                return;
            }
            (void)merge_into_range(moving, 9, 35);
            if (!moving.empty()) {
                (void)merge_into_range(moving, 36, 44);
            }
            context_.containers->set_slot(open_chest_key_, static_cast<std::size_t>(slot), moving);
            send_slot(kChestWindowId, slot, moving);
            send_inventory();
            return;
        }
        const std::size_t pidx = window_to_player_slot(slot);
        auto moving = inventory_.slot(pidx);
        if (moving.empty()) {
            return;
        }
        if (pidx >= 36) {
            (void)merge_into_range(moving, 9, 35);
        } else {
            (void)merge_into_range(moving, 36, 44);
        }
        inventory_.set_slot(pidx, moving);
        send_slot(kChestWindowId, slot, moving);
        send_inventory();
        return;
    }

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
