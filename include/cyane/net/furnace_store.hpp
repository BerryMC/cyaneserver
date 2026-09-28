#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <mutex>
#include <utility>
#include <vector>
#include <unordered_map>

#include "cyane/item/item_stack.hpp"

namespace cyane::net {

// 单个熔炉的运行状态（按方块位置索引）。参照 cuberite cFurnaceEntity：
// 槽位 + 燃烧/冶炼进度存于同一实体，槽位变动即时重估（refill），tick 驱动推进。
struct FurnaceState {
    // 窗口槽位：0=输入 1=燃料 2=产物
    item::ItemStack input;
    item::ItemStack fuel;
    item::ItemStack output;
    std::int32_t burn_left{0};    // 剩余燃烧 tick（0 = 未点燃）
    std::int32_t burn_total{0};   // 本次燃料总 tick（进度条 0/1 用）
    std::int32_t cook_time{0};    // 冶炼进度 0..kCookTicks
    bool lit_synced{false};       // 上次同步到方块点亮位的燃烧标志
};

// 世界熔炉存储：线程安全。Server::tick 驱动冶炼推进，连接线程读写槽位。
// 冶炼表/燃料表为 item id → 映射，由数据文件加载后注入。
class FurnaceStore {
public:
    using FuelMap = std::unordered_map<std::int16_t, std::int32_t>;
    struct SmeltEntry {
        std::int16_t result{0};
        std::uint8_t count{1};
    };
    using SmeltMap = std::unordered_map<std::int16_t, SmeltEntry>;

    void ensure(std::int64_t pos_key) {
        std::lock_guard<std::mutex> lock(mutex_);
        furnaces_.try_emplace(pos_key);
    }

    void remove(std::int64_t pos_key) {
        std::lock_guard<std::mutex> lock(mutex_);
        furnaces_.erase(pos_key);
    }

    [[nodiscard]] bool exists(std::int64_t pos_key) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return furnaces_.contains(pos_key);
    }

    [[nodiscard]] FurnaceState snapshot(std::int64_t pos_key) const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (auto it = furnaces_.find(pos_key); it != furnaces_.end()) {
            return it->second;
        }
        return {};
    }

    // 槽位读写（index 0/1/2）。写后即时重估点燃/冶炼（cuberite OnSlotChanged）。
    void set_slot(std::int64_t pos_key, std::size_t index, item::ItemStack item) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = furnaces_.find(pos_key);
        if (it == furnaces_.end() || index > 2) {
            return;
        }
        switch (index) {
            case 0: it->second.input = std::move(item); break;
            case 1: it->second.fuel = std::move(item); break;
            default: it->second.output = std::move(item); break;
        }
        refill(it->second);
    }

    [[nodiscard]] item::ItemStack slot(std::int64_t pos_key, std::size_t index) const {
        const auto state = snapshot(pos_key);
        switch (index) {
            case 0: return state.input;
            case 1: return state.fuel;
            default: return state.output;
        }
    }

    [[nodiscard]] bool is_fuel(std::int16_t item_id) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return fuel_.contains(item_id);
    }

    [[nodiscard]] bool is_smeltable(std::int16_t item_id) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return smelting_.contains(item_id);
    }

    item::ItemStack merge_into_furnace(std::int64_t pos_key, std::size_t fslot, item::ItemStack moving) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = furnaces_.find(pos_key);
        if (it == furnaces_.end() || fslot > 1 || moving.empty()) {
            return moving;
        }
        auto& target = fslot == 0 ? it->second.input : it->second.fuel;
        if (target.empty()) {
            target = moving;
            moving = item::ItemStack::air();
        } else if (target.stacks_with(moving) && target.count < item::kMaxStack) {
            const auto room = static_cast<int>(item::kMaxStack) - target.count;
            const auto take = static_cast<std::uint8_t>(std::min(room, static_cast<int>(moving.count)));
            target.count = static_cast<std::uint8_t>(target.count + take);
            moving.count = static_cast<std::uint8_t>(moving.count - take);
            if (moving.count == 0) {
                moving = item::ItemStack::air();
            }
        }
        refill(it->second);
        return moving;
    }

    // 注入数据表（启动时调用一次）
    void set_tables(FuelMap fuel, SmeltMap smelting) {
        std::lock_guard<std::mutex> lock(mutex_);
        fuel_ = std::move(fuel);
        smelting_ = std::move(smelting);
    }

    // 恢复存档状态（不做 refill 重估，lit_synced 归零由 tick 重新同步点亮位）
    void restore(std::int64_t pos_key, FurnaceState state) {
        std::lock_guard<std::mutex> lock(mutex_);
        state.lit_synced = false;
        furnaces_[pos_key] = std::move(state);
    }

    // 全量快照（存档）
    [[nodiscard]] std::vector<std::pair<std::int64_t, FurnaceState>> all() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return {furnaces_.begin(), furnaces_.end()};
    }

    // 推进所有熔炉一个 tick（20Hz 调用）
    void tick() {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& [key, f] : furnaces_) {
            tick_one(f);
            const bool lit = f.burn_left > 0;
            if (lit != f.lit_synced) {
                f.lit_synced = lit;
                lit_changes_.emplace_back(key, lit);
            }
        }
    }

    // 取走自上次调用以来发生的燃烧状态翻转（方块 meta 点亮位 8）
    [[nodiscard]] std::vector<std::pair<std::int64_t, bool>> take_lit_changes() {
        std::lock_guard<std::mutex> lock(mutex_);
        return std::exchange(lit_changes_, {});
    }

    static constexpr std::int32_t kCookTicks = 200;

private:
    // 可冶炼：有输入、有对应产物、且产物槽能容纳（空 或 同物可叠到 64）
    [[nodiscard]] bool can_smelt(const FurnaceState& f) const {
        if (f.input.empty()) {
            return false;
        }
        const auto it = smelting_.find(f.input.id);
        if (it == smelting_.end()) {
            return false;
        }
        if (f.output.empty()) {
            return true;
        }
        return f.output.id == it->second.result && f.output.damage == 0 &&
               f.output.count + it->second.count <= item::kMaxStack;
    }

    [[nodiscard]] std::int32_t fuel_burn_time(const item::ItemStack& fuel) const {
        if (fuel.empty()) {
            return 0;
        }
        const auto it = fuel_.find(fuel.id);
        return it != fuel_.end() ? it->second : 0;
    }

    // 槽位变动后即时重估点燃：可冶炼且未点燃且有燃料 → 点火（消耗 1 燃料）。
    // 不重置 cook_time（进度条在无法冶炼时由 tick 自然衰减，而非清零）。
    void refill(FurnaceState& f) {
        if (!can_smelt(f) || f.burn_left > 0) {
            return;
        }
        const std::int32_t burn = fuel_burn_time(f.fuel);
        if (burn > 0) {
            f.burn_total = burn;
            f.burn_left = burn;
            --f.fuel.count;
            if (f.fuel.count == 0) {
                f.fuel = item::ItemStack::air();
            }
        }
    }

    void tick_one(FurnaceState& f) {
        if (f.burn_left > 0) {
            --f.burn_left;
            if (can_smelt(f)) {
                if (++f.cook_time >= kCookTicks) {
                    f.cook_time = 0;
                    const auto& r = smelting_.at(f.input.id);
                    if (f.output.empty()) {
                        f.output = item::ItemStack{r.result, r.count, 0};
                    } else {
                        f.output.count = static_cast<std::uint8_t>(
                            std::min<int>(f.output.count + r.count, item::kMaxStack));
                    }
                    --f.input.count;
                    if (f.input.count == 0) {
                        f.input = item::ItemStack::air();
                    }
                }
            } else if (f.cook_time > 0) {
                --f.cook_time;
            }
        } else if (f.cook_time > 0) {
            --f.cook_time;
        }
        refill(f);
    }

    mutable std::mutex mutex_;
    std::unordered_map<std::int64_t, FurnaceState> furnaces_;
    FuelMap fuel_;
    SmeltMap smelting_;
    std::vector<std::pair<std::int64_t, bool>> lit_changes_;
};

}
