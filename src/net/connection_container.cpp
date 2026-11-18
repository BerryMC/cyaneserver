#include "cyane/net/connection.hpp"

#include <algorithm>
#include <utility>
#include <vector>

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

void Connection::open_small_container(std::int64_t key, ContainerStore::SmallKind kind) {
    if (context_.containers == nullptr) {
        return;
    }
    const auto container = context_.containers->snapshot_small(key);
    if (container.kind != kind) {
        return;  // 存储类型与请求不符（容器表损坏时防御）
    }
    open_small_key_ = key;
    small_open_ = true;
    small_slots_ = kind == ContainerStore::SmallKind::hopper ? ContainerStore::kHopperSlots
                                                             : ContainerStore::kSmallSlots;

    ByteWriter open;
    open.u8(kSmallWindowId);
    open.string(kind == ContainerStore::SmallKind::hopper      ? "minecraft:hopper"
                : kind == ContainerStore::SmallKind::dropper ? "minecraft:dropper"
                                                              : "minecraft:dispenser");
    open.string(proto::chat_text(kind == ContainerStore::SmallKind::hopper      ? "Hopper"
                                 : kind == ContainerStore::SmallKind::dropper ? "Dropper"
                                                                               : "Dispenser"));
    open.u8(static_cast<std::uint8_t>(small_slots_));
    send_packet(proto::play_cb::kOpenWindow, open.data());

    // WindowItems：容器格 + 主背包 27 + 热区栏 9
    const std::int16_t window_slots = static_cast<std::int16_t>(small_slots_ + 36);
    ByteWriter items;
    items.u8(kSmallWindowId);
    items.i16(window_slots);
    for (std::size_t i = 0; i < small_slots_; ++i) {
        item::write_slot(items, container.slots[i]);
    }
    for (std::int16_t i = static_cast<std::int16_t>(small_slots_); i < window_slots; ++i) {
        item::write_slot(items, inventory_.slot(detail::container_window_to_player_slot(
                                    i, static_cast<std::int16_t>(small_slots_))));
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
            const auto [vx, vy, vz] = net::throw_velocity();
            drop_stack(player_pos_.x, player_pos_.y - 0.3 + 1.62, player_pos_.z, leftover,
                       vx, vy, vz, 40);
        }
        cursor_item_ = item::ItemStack::air();
    }
}

bool Connection::handle_play_close_window(ByteSpan payload) {
    (void)payload;
    // CloseWindow (sb 0x08)：byte windowId。客户端关闭容器窗口时发。
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
    small_open_ = false;
    open_small_key_ = 0;
    small_slots_ = 0;
    // 关窗时游标物品退回背包，放不下的部分生成掉落物（避免物品凭空消失）
    if (!cursor_item_.empty()) {
        const item::ItemStack leftover = give_item(cursor_item_);
        if (!leftover.empty()) {
            const auto [vx, vy, vz] = net::throw_velocity();
            drop_stack(player_pos_.x, player_pos_.y - 0.3 + 1.62, player_pos_.z, leftover,
                       vx, vy, vz, 40);
        }
        cursor_item_ = item::ItemStack::air();
    }
    return true;
}

item::ItemStack Connection::apply_chest_click(std::int16_t slot, std::uint8_t button,
                                             std::int32_t mode,
                                             const item::ItemStack& clicked) {
    if (context_.containers == nullptr || (!chest_open_ && !small_open_)) {
        return item::ItemStack::air();
    }
    // 箱子（27 格）与小容器（发射器/投掷器 9 格、漏斗 5 格）共用布局：
    // 容器槽 0..N-1，随后主背包 27（玩家 9..35）、热区栏 9（玩家 36..44）
    const bool is_small = small_open_;
    const auto container_slots = is_small ? static_cast<std::int16_t>(small_slots_)
                                          : static_cast<std::int16_t>(kChestSlots);
    const auto window_slots = static_cast<std::int16_t>(container_slots + 36);
    const std::int64_t ckey = is_small ? open_small_key_ : open_chest_key_;
    const auto win_id = static_cast<std::uint8_t>(is_small ? kSmallWindowId : kChestWindowId);

    const auto at = [&](std::int16_t i) -> item::ItemStack {
        if (i < container_slots) {
            return is_small ? context_.containers->small_slot(ckey, static_cast<std::size_t>(i))
                            : context_.containers->slot(ckey, static_cast<std::size_t>(i));
        }
        return inventory_.slot(detail::container_window_to_player_slot(i, container_slots));
    };
    const auto put = [&](std::int16_t i, const item::ItemStack& value) {
        if (i < container_slots) {
            if (is_small) {
                context_.containers->set_small_slot(ckey, static_cast<std::size_t>(i), value);
            } else {
                context_.containers->set_slot(ckey, static_cast<std::size_t>(i), value);
            }
        } else {
            inventory_.set_slot(detail::container_window_to_player_slot(i, container_slots), value);
        }
    };

    // Container.mergeItemStack：先叠加同类，再填第一个空中槽；reverse 时从 end-1 递减
    const auto merge_into = [&](item::ItemStack& moving, std::int16_t begin, std::int16_t end,
                                bool reverse) {
        const std::int16_t step = reverse ? -1 : 1;
        const auto first = reverse ? static_cast<std::int16_t>(end - 1) : begin;
        const auto past = reverse ? static_cast<std::int16_t>(begin - 1) : end;
        for (auto i = first; i != past && !moving.empty(); i = static_cast<std::int16_t>(i + step)) {
            auto dst = at(i);
            if (dst.empty() || !dst.stacks_with(moving) || dst.count >= item::kMaxStack) continue;
            const int total = static_cast<int>(dst.count) + static_cast<int>(moving.count);
            dst.count = static_cast<std::uint8_t>(std::min(total, static_cast<int>(item::kMaxStack)));
            moving.count = static_cast<std::uint8_t>(total - dst.count);
            if (moving.count == 0) moving = item::ItemStack::air();
            put(i, dst);
        }
        if (moving.empty()) return;
        for (auto i = first; i != past; i = static_cast<std::int16_t>(i + step)) {
            if (!at(i).empty()) continue;
            put(i, moving);
            moving = item::ItemStack::air();
            return;
        }
    };

    struct Ops {
        Connection& self;
        std::int16_t container_slots;
        std::int16_t window_slots;
        const decltype(at)& at_fn;
        const decltype(put)& put_fn;
        const decltype(merge_into)& merge_fn;

        [[nodiscard]] std::int16_t slot_count() const noexcept { return window_slots; }
        [[nodiscard]] item::ItemStack get(std::int16_t s) const { return at_fn(s); }
        void set(std::int16_t s, item::ItemStack stack) { put_fn(s, stack); }
        [[nodiscard]] std::uint8_t stack_limit(std::int16_t, const item::ItemStack&) const noexcept {
            return item::kMaxStack;
        }
        // QUICK_MOVE：容器槽 → 玩家（reverse），玩家槽 → 容器（forward）
        [[nodiscard]] item::ItemStack transfer(std::int16_t s) {
            auto moving = get(s);
            if (moving.empty()) return moving;
            if (s < container_slots) {
                merge_fn(moving, container_slots, window_slots, true);
            } else {
                merge_fn(moving, 0, container_slots, false);
            }
            set(s, moving);
            return moving;
        }
        void drop(item::ItemStack stack, bool) {
            if (stack.empty()) return;
            const auto [vx, vy, vz] = net::throw_velocity();
            self.drop_stack(self.player_pos_.x, self.player_pos_.y - 0.3 + 1.62,
                            self.player_pos_.z, stack, vx, vy, vz, 40);
        }
        [[nodiscard]] bool add_to_inventory(item::ItemStack stack) {
            return self.give_item(stack).empty();
        }
    };
    Ops ops{*this, container_slots, window_slots, at, put, merge_into};

    // 创造模式：光标为空且点了窗口外时，采信客户端声明（原版创造选择器取出流程）
    if (context_.game_mode == proto::game_mode::kCreative && cursor_item_.empty() &&
        !clicked.empty() && mode == click_type::kPickup && slot == -999) {
        cursor_item_ = clicked;
    }

    const auto expected = slot_click(ops, cursor_item_, drag_state_, mode, slot,
                                     static_cast<std::int8_t>(button),
                                     context_.game_mode == proto::game_mode::kCreative);

    // 全窗重发：一次性把容器与玩家的权威状态同步给客户端
    ByteWriter items;
    items.u8(win_id);
    items.i16(window_slots);
    for (std::int16_t i = 0; i < window_slots; ++i) {
        item::write_slot(items, at(i));
    }
    send_packet(proto::play_cb::kWindowItems, items.data());
    send_slot(-1, -1, cursor_item_);
    return expected;
}

}
