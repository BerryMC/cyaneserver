#include "cyane/net/connection.hpp"
#include "cyane/core/log.hpp"

#include <algorithm>

#include "connection_detail.hpp"
#include "cyane/proto/json.hpp"

namespace cyane::net {

namespace {
constexpr std::string_view kTableWindowType = "minecraft:crafting_table";
constexpr std::int16_t kTableGridCells = 9;
// 工作台窗口玩家侧槽位：主背包 9..35 + 热区栏 36..44，共 36（背包本体另有 0..8 合成与手持）
constexpr std::int16_t kTablePlayerSlots = 36;
}

// 1.12.2 工作台窗口：windowId=4、类型 "minecraft:crafting_table"。
// OpenWindow 槽数=0（客户端据此渲染 3×3 GuiCrafting）；客户端容器实际 46 槽
// （0-8 网格、9 结果、10-36 主背包、37-45 热区栏），与 apply_table_click 映射一致。
// 格内容持久化在 CraftingTableStore，结果即时计算不存储。
void Connection::open_crafting_table(std::int64_t table_key) {
    if (context_.crafting_tables == nullptr) {
        return;
    }
    table_open_ = true;
    open_table_key_ = table_key;
    table_grid_ = context_.crafting_tables->cells(table_key);
    table_result_ = compute_table_result();

    // OpenWindow (0x13)：byte windowId | string windowType | chat title | ubyte slotCount
    // slotCount=0：1.12.2 客户端据此走 displayGui(crafting_table) → 3×3 GuiCrafting；
    // 非 0 会落入通用容器（GuiChest），网格渲染成单排。
    ByteWriter open;
    open.u8(kCraftingTableWindowId);
    open.string(kTableWindowType);
    open.string(proto::chat_text("Crafting"));
    open.u8(0);
    send_packet(proto::play_cb::kOpenWindow, open.data());

    // WindowItems (0x14)：46 槽（网格 9 + 结果 1 + 主背包 27 + 热区栏 9），同 NMS updateInventory
    ByteWriter items;
    items.u8(kCraftingTableWindowId);
    items.i16(kTableGridCells + 1 + kTablePlayerSlots);
    for (const auto& cell : table_grid_) {
        item::write_slot(items, cell);
    }
    item::write_slot(items, table_result_);
    for (std::size_t idx = 9; idx < 9 + kTablePlayerSlots; ++idx) {
        item::write_slot(items, inventory_.slot(idx));
    }
    send_packet(proto::play_cb::kWindowItems, items.data());
}

item::ItemStack Connection::compute_table_result() const {
    if (context_.crafting == nullptr) {
        return {};
    }
    if (const auto recipe = context_.crafting->find(table_grid_, 3, 3); recipe) {
        return {recipe->result_id, recipe->result_count, 0};
    }
    return {};
}

// 消耗一格材料：每个非空格 -1
void Connection::consume_table_materials() {
    for (std::size_t i = 0; i < kTableGridCells; ++i) {
        if (table_grid_[i].empty()) {
            continue;
        }
        auto cell = table_grid_[i];
        if (--cell.count == 0) {
            cell = item::ItemStack::air();
        }
        table_grid_[i] = cell;
        context_.crafting_tables->set_cell(open_table_key_, i, cell);
    }
    table_result_ = compute_table_result();
}

void Connection::apply_table_click(std::int16_t slot, std::uint8_t button, std::int32_t mode,
                                   const item::ItemStack& clicked) {
    if (context_.crafting_tables == nullptr || !table_open_) {
        return;
    }
    // 创造模式：客户端经 clickedItem 声明创造选择器取出的物品；服务端游标为空时采信
    if (context_.game_mode == proto::game_mode::kCreative && cursor_item_.empty() && !clicked.empty()) {
        cursor_item_ = clicked;
    }
    // 工作台容器自身 10 槽（0-8 格、9 结果）；客户端底栏/主背包点击以窗口 4、
    // slot 10..45 上报（10..36 主背包 9..35，37..45 热区栏 36..44）
    if (slot >= 10 && slot <= 45) {
        // slot 10..36 → 主背包 9..35；slot 37..45 → 热区栏 36..44（idx = slot - 1）
        const std::size_t idx = static_cast<std::size_t>(slot - 1);
        item::ItemStack in_slot = inventory_.slot(idx);
        if (mode == 1) {
            // shift：热区栏 → 主背包，主背包 → 热区栏
            if (!in_slot.empty()) {
                auto moving = in_slot;
                if (idx >= 36) {
                    (void)merge_into_range(moving, 9, 35);
                } else {
                    (void)merge_into_range(moving, 36, 44);
                }
                in_slot = moving;
                inventory_.set_slot(idx, in_slot);
                send_slot(kCraftingTableWindowId, slot, in_slot);
                send_slot(0, static_cast<std::int16_t>(idx), in_slot);
                return;
            }
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
        }
        inventory_.set_slot(idx, in_slot);
        send_slot(kCraftingTableWindowId, slot, in_slot);
        send_slot(0, static_cast<std::int16_t>(idx), in_slot);
        send_slot(-1, -1, cursor_item_);
        return;
    }
    if (slot < 0 || slot >= kTableGridCells + 1) {
        if (slot < 0) {
            cursor_item_ = item::ItemStack::air();
        }
        return;
    }

    if (slot == 9) {
        // 结果槽：左/右键取走整份产物（消耗一份材料），shift 自动进背包
        if (table_result_.empty()) {
            return;
        }
        const auto result = table_result_;
        if (mode == 1) {
            consume_table_materials();
            const item::ItemStack leftover = give_item(result);
            if (!leftover.empty()) {
                const auto cpos = world::ChunkPos::from_world(static_cast<std::int32_t>(player_pos_.x),
                                                               static_cast<std::int32_t>(player_pos_.z));
                drop_stack(player_pos_.x, player_pos_.y, player_pos_.z, leftover,
                           cpos ? cpos->x : 0, cpos ? cpos->z : 0);
            }
        } else {
            const int total = static_cast<int>(cursor_item_.count) + static_cast<int>(result.count);
            if (cursor_item_.empty() || (cursor_item_.stacks_with(result) && total <= item::kMaxStack)) {
                consume_table_materials();
                if (cursor_item_.empty()) {
                    cursor_item_ = result;
                } else {
                    cursor_item_.count = static_cast<std::uint8_t>(total);
                }
            }
        }
        send_slot(kCraftingTableWindowId, 9, table_result_);
        send_slot(-1, -1, cursor_item_);
        return;
    }

    auto& cell = table_grid_[static_cast<std::size_t>(slot)];

    if (mode == 1) {
        // shift：整叠进玩家背包；放不下的部分留在原格
        if (cell.empty()) {
            return;
        }
        auto moving = cell;
        cell = item::ItemStack::air();
        context_.crafting_tables->set_cell(open_table_key_, static_cast<std::size_t>(slot), cell);
        const item::ItemStack leftover = give_item(moving);
        if (!leftover.empty()) {
            cell = leftover;
            context_.crafting_tables->set_cell(open_table_key_, static_cast<std::size_t>(slot), cell);
        }
        table_result_ = compute_table_result();
        send_slot(kCraftingTableWindowId, slot, cell);
        send_slot(kCraftingTableWindowId, 9, table_result_);
        send_slot(-1, -1, cursor_item_);
        return;
    }

    auto refresh_result = [this]() {
        const auto fresh = compute_table_result();
        if (fresh.id != table_result_.id || fresh.count != table_result_.count) {
            table_result_ = fresh;
            send_slot(kCraftingTableWindowId, 9, table_result_);
        }
    };

    if (button == 0) {
        // 左键：叠放或互换
        if (!cell.empty() && !cursor_item_.empty() && cursor_item_.stacks_with(cell)) {
            const int total = static_cast<int>(cell.count) + static_cast<int>(cursor_item_.count);
            const int keep = std::min(total, static_cast<int>(item::kMaxStack));
            cell.count = static_cast<std::uint8_t>(keep);
            cursor_item_.count = static_cast<std::uint8_t>(total - keep);
            if (cursor_item_.count == 0) {
                cursor_item_ = item::ItemStack::air();
            }
        } else {
            std::swap(cell, cursor_item_);
        }
        context_.crafting_tables->set_cell(open_table_key_, static_cast<std::size_t>(slot), cell);
        refresh_result();
        send_slot(kCraftingTableWindowId, slot, cell);
        send_slot(-1, -1, cursor_item_);
        return;
    }

    if (button == 1) {
        // 右键：空格放 1 个，否则取一半
        if (cell.empty()) {
            if (!cursor_item_.empty()) {
                cell = cursor_item_;
                --cell.count;
                if (cell.count == 0) {
                    cursor_item_ = item::ItemStack::air();
                } else {
                    cursor_item_.count = cell.count;
                }
                context_.crafting_tables->set_cell(open_table_key_, static_cast<std::size_t>(slot), cell);
                refresh_result();
                send_slot(kCraftingTableWindowId, slot, cell);
                send_slot(-1, -1, cursor_item_);
            }
            return;
        }
        const std::uint8_t take = static_cast<std::uint8_t>((cell.count + 1) / 2);
        if (!cursor_item_.empty()) {
            const int total = static_cast<int>(cursor_item_.count) + static_cast<int>(take);
            if (!(cursor_item_.stacks_with(cell) && total <= item::kMaxStack)) {
                return;
            }
            cursor_item_.count = static_cast<std::uint8_t>(total);
            cell.count = static_cast<std::uint8_t>(cell.count - take);
            if (cell.count == 0) {
                cell = item::ItemStack::air();
            }
            context_.crafting_tables->set_cell(open_table_key_, static_cast<std::size_t>(slot), cell);
            refresh_result();
            send_slot(kCraftingTableWindowId, slot, cell);
            send_slot(-1, -1, cursor_item_);
            return;
        }
        cursor_item_ = cell;
        cursor_item_.count = take;
        cell.count = static_cast<std::uint8_t>(cell.count - take);
        if (cell.count == 0) {
            cell = item::ItemStack::air();
        }
        context_.crafting_tables->set_cell(open_table_key_, static_cast<std::size_t>(slot), cell);
        refresh_result();
        send_slot(kCraftingTableWindowId, slot, cell);
        send_slot(-1, -1, cursor_item_);
    }
}

} // namespace cyane::net
