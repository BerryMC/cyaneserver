#include "cyane/net/connection.hpp"

#include <algorithm>

#include "connection_detail.hpp"
#include "cyane/core/log.hpp"
#include "cyane/proto/json.hpp"

namespace cyane::net {

namespace {
// 熔炉窗口布局：0=输入 1=燃料 2=产物，3..29 主背包(玩家 9..35)，30..38 热区栏(玩家 36..44)
using detail::kFurnaceSlots;
using detail::kFurnaceWindowSlots;

[[nodiscard]] std::size_t furnace_window_to_player_slot(std::int16_t win_slot) noexcept {
    return detail::container_window_to_player_slot(win_slot, kFurnaceSlots);
}
}

void Connection::open_furnace(std::int64_t furnace_key) {
    if (context_.furnaces == nullptr) {
        return;
    }
    if (!context_.furnaces->exists(furnace_key)) {
        context_.furnaces->ensure(furnace_key);
    }
    open_furnace_key_ = furnace_key;
    furnace_open_ = true;
    const auto state = context_.furnaces->snapshot(furnace_key);
    last_burn_left_ = state.burn_left;
    last_cook_time_ = state.cook_time;
    last_furnace_slots_ = {state.input, state.fuel, state.output};

    // OpenWindow (0x13)：byte windowId | string windowType | chat title | ubyte slotCount
    ByteWriter open;
    open.u8(kFurnaceWindowId);
    open.string("minecraft:furnace");
    open.string(proto::chat_text("Furnace"));
    open.u8(static_cast<std::uint8_t>(kFurnaceSlots));
    send_packet(proto::play_cb::kOpenWindow, open.data());

    // WindowItems (0x14)：39 槽（熔炉 3 + 主背包 27 + 热区栏 9）
    ByteWriter items;
    items.u8(kFurnaceWindowId);
    items.i16(kFurnaceWindowSlots);
    item::write_slot(items, state.input);
    item::write_slot(items, state.fuel);
    item::write_slot(items, state.output);
    for (std::int16_t i = kFurnaceSlots; i < kFurnaceWindowSlots; ++i) {
        item::write_slot(items, inventory_.slot(furnace_window_to_player_slot(i)));
    }
    send_packet(proto::play_cb::kWindowItems, items.data());

    // 开窗即强制同步进度条（0=剩余燃烧 1=燃烧总值 2=冶炼进度 3=冶炼总值），
    // 客户端据此渲染火焰与进度箭头
    send_furnace_progress(true);
}

void Connection::send_furnace_progress(bool force) {
    if (context_.furnaces == nullptr || !furnace_open_) {
        return;
    }
    const auto state = context_.furnaces->snapshot(open_furnace_key_);
    const auto write_prop = [this](std::int16_t prop, std::int16_t value) {
        ByteWriter w;
        w.u8(kFurnaceWindowId);
        w.i16(prop);
        w.i16(value);
        send_packet(proto::play_cb::kWindowProperty, w.data());
    };
    if (force || state.burn_left != last_burn_left_) {
        last_burn_left_ = state.burn_left;
        write_prop(0, static_cast<std::int16_t>(std::min<std::int32_t>(state.burn_left, 0x7FFF)));
        write_prop(1, static_cast<std::int16_t>(std::min<std::int32_t>(state.burn_total, 0x7FFF)));
    }
    if (force || state.cook_time != last_cook_time_) {
        last_cook_time_ = state.cook_time;
        write_prop(2, static_cast<std::int16_t>(state.cook_time));
        write_prop(3, static_cast<std::int16_t>(FurnaceStore::kCookTicks));
    }
}

// 冶炼推进改变输入/燃料/产物槽时，已开窗客户端需要 SetSlot 才看得见结果
void Connection::sync_furnace_progress() {
    if (context_.furnaces == nullptr || !furnace_open_) {
        return;
    }
    send_furnace_progress(false);
    const auto state = context_.furnaces->snapshot(open_furnace_key_);
    if (state.input.id != last_furnace_slots_[0].id || state.input.count != last_furnace_slots_[0].count ||
        state.fuel.id != last_furnace_slots_[1].id || state.fuel.count != last_furnace_slots_[1].count ||
        state.output.id != last_furnace_slots_[2].id || state.output.count != last_furnace_slots_[2].count) {
        last_furnace_slots_ = {state.input, state.fuel, state.output};
        send_slot(kFurnaceWindowId, 0, state.input);
        send_slot(kFurnaceWindowId, 1, state.fuel);
        send_slot(kFurnaceWindowId, 2, state.output);
    }
}

// 把熔炉某槽的当前物品写回权威存储（即时持久化 + 触发反应式重估），并回发该槽 + 游标
void Connection::commit_furnace_slot(std::size_t fslot, const item::ItemStack& in_slot) {
    context_.furnaces->set_slot(open_furnace_key_, fslot, in_slot);
    last_furnace_slots_[fslot] = in_slot;
    send_slot(kFurnaceWindowId, static_cast<std::int16_t>(fslot), in_slot);
}

// 存储侧更新（refill 可能消耗燃料、tick 可能产出）后回读权威值补发三槽 + 进度条
void Connection::send_furnace_slots_now() {
    if (context_.furnaces == nullptr || !furnace_open_) {
        return;
    }
    const auto state = context_.furnaces->snapshot(open_furnace_key_);
    last_furnace_slots_ = {state.input, state.fuel, state.output};
    send_slot(kFurnaceWindowId, 0, state.input);
    send_slot(kFurnaceWindowId, 1, state.fuel);
    send_slot(kFurnaceWindowId, 2, state.output);
    send_furnace_progress(true);
}

void Connection::apply_furnace_click(std::int16_t slot, std::uint8_t button, std::int32_t mode,
                                     const item::ItemStack& clicked) {
    if (context_.furnaces == nullptr || !furnace_open_) {
        return;
    }
    if (!context_.furnaces->exists(open_furnace_key_)) {
        context_.furnaces->ensure(open_furnace_key_);
    }
    // 创造模式：客户端经 clickedItem 声明创造选择器取出的物品；服务端游标为空时采信
    if (context_.game_mode == proto::game_mode::kCreative && cursor_item_.empty() && !clicked.empty()) {
        cursor_item_ = clicked;
    }
    if (slot < 0 || slot >= kFurnaceWindowSlots) {
        if (slot < 0) {
            cursor_item_ = item::ItemStack::air();
        }
        return;
    }

    // 玩家背包槽（3..38）：shift 把整叠送进熔炉，其余走普通拿放
    if (slot >= kFurnaceSlots) {
        const std::size_t idx = furnace_window_to_player_slot(slot);
        item::ItemStack in_slot = inventory_.slot(idx);
        if (mode == 1 && !in_slot.empty()) {
            // 参照 Cuberite cSlotAreaFurnace::DistributeStack：可冶炼→输入槽，
            // 否则是燃料→燃料槽，都不是就只在背包内转移（保持原版不吞物品）
            std::optional<std::size_t> target;
            if (context_.furnaces->is_smeltable(in_slot.id)) {
                target = 0;
            } else if (context_.furnaces->is_fuel(in_slot.id)) {
                target = 1;
            }
            if (target) {
                in_slot = context_.furnaces->merge_into_furnace(open_furnace_key_, *target, in_slot);
                inventory_.set_slot(idx, in_slot);
                send_slot(kFurnaceWindowId, slot, in_slot);
                send_slot(0, static_cast<std::int16_t>(idx), in_slot);
                // refill 可能已消耗一份燃料，槽内容要回读权威值
                send_furnace_slots_now();
                return;
            }
            auto moving = in_slot;
            if (idx >= 36 && idx < 45) {
                (void)merge_into_range(moving, 9, 35);
            } else {
                (void)merge_into_range(moving, 36, 44);
            }
            inventory_.set_slot(idx, moving);
            send_slot(kFurnaceWindowId, slot, moving);
            send_slot(0, static_cast<std::int16_t>(idx), moving);
            return;
        }
        if (button == 0) {
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
        inventory_.set_slot(idx, in_slot);
        send_slot(static_cast<std::int8_t>(kFurnaceWindowId), slot, in_slot);
        send_slot(-1, -1, cursor_item_);
        return;
    }

    // 熔炉槽：0=输入 1=燃料 2=产物
    const std::size_t fslot = static_cast<std::size_t>(slot);
    item::ItemStack in_slot = context_.furnaces->slot(open_furnace_key_, fslot);

    if (mode == 1) {
        // shift：熔炉槽整叠移入玩家背包（主背包→热区栏），余量留在原槽（不落地，与原版一致）
        if (in_slot.empty()) {
            return;
        }
        auto moving = in_slot;
        (void)merge_into_range(moving, 9, 35);
        if (!moving.empty()) {
            (void)merge_into_range(moving, 36, 44);
        }
        commit_furnace_slot(fslot, moving);
        send_slot(-1, -1, cursor_item_);
        return;
    }

    if (fslot == 2) {
        // 产物槽只允许拿取：左键整叠，右键一半（不能放入）
        if (in_slot.empty()) {
            return;
        }
        const std::uint8_t take =
            button == 1 ? static_cast<std::uint8_t>((in_slot.count + 1) / 2) : in_slot.count;
        if (cursor_item_.empty() ||
            (cursor_item_.stacks_with(in_slot) && cursor_item_.count + take <= item::kMaxStack)) {
            if (cursor_item_.empty()) {
                cursor_item_ = in_slot;
                cursor_item_.count = take;
            } else {
                cursor_item_.count = static_cast<std::uint8_t>(cursor_item_.count + take);
            }
            const std::uint8_t left = static_cast<std::uint8_t>(in_slot.count - take);
            commit_furnace_slot(fslot,
                                left == 0 ? item::ItemStack::air() : item::ItemStack{in_slot.id, left, in_slot.damage});
            send_slot(-1, -1, cursor_item_);
        }
        return;
    }

    // 输入/燃料槽：普通拿放（放入 1 个时右键）
    if (button == 0) {
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
    commit_furnace_slot(fslot, in_slot);
    send_slot(-1, -1, cursor_item_);
}

}
