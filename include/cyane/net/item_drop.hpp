#pragma once

#include <cmath>
#include <cstdint>
#include <mutex>
#include <random>
#include <span>
#include <tuple>
#include <vector>

#include "cyane/entity/player_manager.hpp"
#include "cyane/item/item_stack.hpp"
#include "cyane/world/physics.hpp"
#include "cyane/world/world.hpp"

namespace cyane::net {

// 世界中的一个掉落物实体 = vanilla EntityItem（1.12.2 反混淆源码逐行转写）：
// 重力 0.04/tick、地面摩擦 slipperiness×0.98（空气 0.98）、motionY×0.98、
// 落地反弹 ×−0.5、age 6000 tick 消失、pickupDelay 拾取延迟、health 5（火 1/岩浆 4 伤害）、
// 相邻同类堆叠合并（盒 grow 0.5/0/0.5，大者吸收小者）。
struct DroppedItem {
    std::uint32_t entity_id{0};
    double x{0.0};
    double y{0.0};
    double z{0.0};
    double velocity_x{0.0};
    double velocity_y{0.0};
    double velocity_z{0.0};
    item::ItemStack stack;
    std::int32_t age{0};            // EntityItem.age（≥6000 消失）
    std::int32_t pickup_delay{10};  // setDefaultPickupDelay=10；玩家丢弃=40；32767=永不
    std::int32_t health{5};         // EntityItem.health（火/岩浆扣血）
    bool on_ground{false};
    // 位置同步记账（不持久化）：vanilla tracker 每 20 tick 校正一次，漂移 >4 格立即传送
    std::int32_t sync_ticks{0};
    double sync_x{0.0};
    double sync_y{0.0};
    double sync_z{0.0};
};

// 持久化形状：位置 + 堆叠 + 年龄 + 拾取延迟（vanilla NBT 的 Age/PickupDelay 同语义）
struct DroppedItemState {
    double x{0.0};
    double y{0.0};
    double z{0.0};
    item::ItemStack stack;
    std::int32_t age{0};
    std::int32_t pickup_delay{0};
};

// 一次被某玩家拾取的结果：谁拾取、拾取了哪个实体、堆叠内容。
struct PickupEvent {
    std::uint32_t item_entity_id{0};
    std::uint32_t collector_id{0};
    item::ItemStack stack;
    double x{0.0};
    double y{0.0};
    double z{0.0};
};

// 掉落物 tick 的事件：Server 落地为广播（ItemDropManager 不依赖连接与邮箱）
struct ItemDestroyed {
    std::uint32_t entity_id{0};
    double x{0.0};
    double y{0.0};
    double z{0.0};
    bool burn_sound{false};  // 岩浆浮起时的 entity.generic.burn
};
struct ItemMoved {
    std::uint32_t entity_id{0};
    double x{0.0};
    double y{0.0};
    double z{0.0};
};
struct ItemMerged {
    std::uint32_t victim_id{0};
    std::uint32_t survivor_id{0};
    item::ItemStack stack;  // 吸收后的堆叠（重发 metadata）
    double x{0.0};
    double y{0.0};
    double z{0.0};
};
struct ItemSound {
    double x{0.0};
    double y{0.0};
    double z{0.0};
};
struct ItemTickResult {
    std::vector<ItemDestroyed> destroyed;
    std::vector<ItemMoved> moved;
    std::vector<ItemMerged> merged;
    std::vector<ItemSound> burn_sounds;  // entity.generic.burn（岩浆浮起）
};

// 玩家丢弃/死亡的抛掷初速（EntityPlayer.dropItem dropAround 分支）：
// 随机方向 × rand(0..0.5)，竖直 0.2
[[nodiscard]] inline std::tuple<double, double, double> throw_velocity() {
    thread_local std::mt19937 engine{std::random_device{}()};
    std::uniform_real_distribution<double> unit(0.0, 1.0);
    constexpr double kTwoPi = 6.283185307179586;
    const double speed = unit(engine) * 0.5;
    const double angle = unit(engine) * kTwoPi;
    return {-std::sin(angle) * speed, 0.20000000298023224, std::cos(angle) * speed};
}

// Block.spawnAsEntity 的出生点：方块内随机（0.25 + rand×0.5 每轴）
[[nodiscard]] inline std::tuple<double, double, double> in_block_spawn_pos(std::int32_t bx,
                                                                          std::int32_t by,
                                                                          std::int32_t bz) {
    thread_local std::mt19937 engine{std::random_device{}()};
    std::uniform_real_distribution<double> unit(0.0, 1.0);
    return {static_cast<double>(bx) + 0.25 + unit(engine) * 0.5,
            static_cast<double>(by) + 0.25 + unit(engine) * 0.5,
            static_cast<double>(bz) + 0.25 + unit(engine) * 0.5};
}

// 线程安全的掉落物登记表。连接线程 spawn/拾取，Server::tick 驱动物理。
class ItemDropManager {
public:
    // 生成一个掉落物，返回其实体 id（0 表示空堆叠未生成）
    std::uint32_t spawn(double x, double y, double z, item::ItemStack stack, double vx, double vy,
                        double vz, std::int32_t pickup_delay) {
        if (stack.empty()) {
            return 0;
        }
        const std::uint32_t id = entity::allocate_entity_id();
        std::lock_guard<std::mutex> lock{mutex_};
        DroppedItem item;
        item.entity_id = id;
        item.x = x;
        item.y = y;
        item.z = z;
        item.velocity_x = vx;
        item.velocity_y = vy;
        item.velocity_z = vz;
        item.stack = std::move(stack);
        item.pickup_delay = pickup_delay;
        item.sync_x = x;
        item.sync_y = y;
        item.sync_z = z;
        items_.push_back(std::move(item));
        return id;
    }

    // 从存档恢复掉落物（年龄/延迟随存档）
    void restore(std::span<const DroppedItemState> drops) {
        std::lock_guard<std::mutex> lock{mutex_};
        for (const auto& drop : drops) {
            if (drop.stack.empty()) {
                continue;
            }
            DroppedItem item;
            item.entity_id = entity::allocate_entity_id();
            item.x = drop.x;
            item.y = drop.y;
            item.z = drop.z;
            item.stack = drop.stack;
            item.age = drop.age;
            item.pickup_delay = drop.pickup_delay;
            item.sync_x = drop.x;
            item.sync_y = drop.y;
            item.sync_z = drop.z;
            items_.push_back(std::move(item));
        }
    }

    // 全量快照（保存存档用）
    [[nodiscard]] std::vector<DroppedItemState> all_drops() const {
        std::lock_guard<std::mutex> lock{mutex_};
        std::vector<DroppedItemState> out;
        out.reserve(items_.size());
        for (const auto& it : items_) {
            out.push_back(DroppedItemState{it.x, it.y, it.z, it.stack, it.age, it.pickup_delay});
        }
        return out;
    }

    // 当前所有掉落物快照（新玩家/区块补发）
    [[nodiscard]] std::vector<DroppedItem> snapshot() const {
        std::lock_guard<std::mutex> lock{mutex_};
        return items_;
    }

    // 推进所有掉落物一个 tick（EntityItem.onUpdate 逐行转写）
    [[nodiscard]] ItemTickResult tick(world::World& world) {
        ItemTickResult result;
        std::lock_guard<std::mutex> lock{mutex_};
        for (std::size_t i = 0; i < items_.size();) {
            DroppedItem& item = items_[i];
            if (item.stack.empty()) {
                result.destroyed.push_back(ItemDestroyed{item.entity_id, item.x, item.y, item.z});
                items_[i] = items_.back();
                items_.pop_back();
                continue;
            }
            if (item.pickup_delay > 0 && item.pickup_delay != 32767) {
                --item.pickup_delay;
            }
            const double prev_x = item.x;
            const double prev_y = item.y;
            const double prev_z = item.z;
            // 重力 0.04（比生物的 0.08 温和）
            item.velocity_y -= 0.03999999910593033;
            auto box = world::entity_box(item.x, item.y, item.z, 0.25f, 0.25f);
            double dx = item.velocity_x;
            double dy = item.velocity_y;
            double dz = item.velocity_z;
            const auto outcome = world::move_with_collision(world, box, dx, dy, dz, 0.0);
            item.x = (box.min_x + box.max_x) * 0.5;
            item.y = box.min_y;
            item.z = (box.min_z + box.max_z) * 0.5;
            item.on_ground = outcome.on_ground;
            if (outcome.on_ground) {
                item.velocity_y = 0.0;
            }
            // 方块跨越或每 25 tick：岩浆浮起检查 + 相邻合并（vanilla 触发条件）
            const bool crossed_block = static_cast<std::int32_t>(prev_x) !=
                                           static_cast<std::int32_t>(item.x) ||
                                       static_cast<std::int32_t>(prev_y) !=
                                           static_cast<std::int32_t>(item.y) ||
                                       static_cast<std::int32_t>(prev_z) !=
                                           static_cast<std::int32_t>(item.z);
            if (crossed_block || item.age % 25 == 0) {
                const auto here = world::block_id(world.block_at(
                    static_cast<std::int32_t>(std::floor(item.x)),
                    static_cast<std::int32_t>(std::floor(item.y)),
                    static_cast<std::int32_t>(std::floor(item.z))));
                if (here == 10 || here == 11) {  // 岩浆面上弹起 + 燃烧音（vanilla 同）
                    item.velocity_y = 0.20000000298023224;
                    thread_local std::mt19937 engine{std::random_device{}()};
                    std::uniform_real_distribution<float> unit(-1.0f, 1.0f);
                    item.velocity_x = static_cast<double>(unit(engine)) * 0.2;
                    item.velocity_z = static_cast<double>(unit(engine)) * 0.2;
                    result.burn_sounds.push_back(ItemSound{item.x, item.y, item.z});
                }
                combine_with_neighbors(i, result);
                if (item.stack.empty()) {  // 本体被更大的邻居吸收
                    items_[i] = items_.back();
                    items_.pop_back();
                    continue;
                }
            }
            // 摩擦：地面 slipperiness×0.98，空气 0.98；motionY×0.98；落地反弹 −0.5
            float f = 0.98f;
            if (item.on_ground) {
                const auto below = world.block_at(
                    static_cast<std::int32_t>(std::floor(item.x)),
                    static_cast<std::int32_t>(std::floor(item.y)) - 1,
                    static_cast<std::int32_t>(std::floor(item.z)));
                f = static_cast<float>(world::block_slipperiness(below)) * 0.98f;
            }
            item.velocity_x *= static_cast<double>(f);
            item.velocity_z *= static_cast<double>(f);
            item.velocity_y *= 0.9800000190734863;
            if (item.on_ground) {
                item.velocity_y *= -0.5;
            }
            ++item.age;
            // 火焰伤害（Entity.move 的 isFlammableWithin → dealFireDamage(1)）与
            // 岩浆伤害（setOnFireFromLava → LAVA 4.0）
            std::int32_t damage = 0;
            const auto scan = [&](const auto& test) {
                const auto x0 = static_cast<std::int32_t>(std::floor(box.min_x));
                const auto x1 = static_cast<std::int32_t>(std::floor(box.max_x));
                const auto y0 = static_cast<std::int32_t>(std::floor(box.min_y));
                const auto y1 = static_cast<std::int32_t>(std::floor(box.max_y));
                const auto z0 = static_cast<std::int32_t>(std::floor(box.min_z));
                const auto z1 = static_cast<std::int32_t>(std::floor(box.max_z));
                for (auto sy = y0; sy <= y1; ++sy) {
                    for (auto sz = z0; sz <= z1; ++sz) {
                        for (auto sx = x0; sx <= x1; ++sx) {
                            if (test(world::block_id(world.block_at(sx, sy, sz)))) {
                                return true;
                            }
                        }
                    }
                }
                return false;
            };
            if (scan([](std::uint16_t id) { return id == 51; })) {  // 火
                damage += 1;
            }
            if (scan([](std::uint16_t id) { return id == 10 || id == 11; })) {  // 岩浆
                damage += 4;
            }
            if (damage > 0) {
                item.health -= damage;
                if (item.health <= 0) {
                    result.destroyed.push_back(ItemDestroyed{item.entity_id, item.x, item.y,
                                                             item.z, damage >= 4});
                    items_[i] = items_.back();
                    items_.pop_back();
                    continue;
                }
            }
            if (item.age >= 6000) {  // lifespan：5 分钟消失
                result.destroyed.push_back(ItemDestroyed{item.entity_id, item.x, item.y, item.z});
                items_[i] = items_.back();
                items_.pop_back();
                continue;
            }
            // 位置同步：vanilla tracker 对掉落物每 20 tick 校正一次（漂移 >4 立即传）
            ++item.sync_ticks;
            const double drift_sq = (item.x - item.sync_x) * (item.x - item.sync_x) +
                                    (item.y - item.sync_y) * (item.y - item.sync_y) +
                                    (item.z - item.sync_z) * (item.z - item.sync_z);
            if (drift_sq > 16.0 || (item.sync_ticks >= 20 && drift_sq > 1e-8)) {
                result.moved.push_back(ItemMoved{item.entity_id, item.x, item.y, item.z});
                item.sync_ticks = 0;
                item.sync_x = item.x;
                item.sync_y = item.y;
                item.sync_z = item.z;
            }
            ++i;
        }
        return result;
    }

    // vanilla 拾取（EntityPlayer.onLivingUpdate 扫描盒 grow(1,0.5,1) → onCollideWithPlayer）：
    // 扫描盒与物品盒（0.25）相交且拾取延迟已过 → 原子取走
    [[nodiscard]] std::vector<PickupEvent> collect_overlapping(std::uint32_t collector,
                                                               const world::Aabb& scan_box) {
        std::vector<PickupEvent> picked;
        std::lock_guard<std::mutex> lock{mutex_};
        for (std::size_t i = 0; i < items_.size();) {
            const DroppedItem& it = items_[i];
            const world::Aabb item_box{it.x - 0.125, it.y, it.z - 0.125, it.x + 0.125,
                                       it.y + 0.25, it.z + 0.125};
            const bool overlap = scan_box.min_x <= item_box.max_x &&
                                 scan_box.max_x >= item_box.min_x &&
                                 scan_box.min_y <= item_box.max_y &&
                                 scan_box.max_y >= item_box.min_y &&
                                 scan_box.min_z <= item_box.max_z &&
                                 scan_box.max_z >= item_box.min_z;
            if (overlap && it.pickup_delay <= 0) {
                picked.push_back(PickupEvent{it.entity_id, collector, it.stack, it.x, it.y, it.z});
                items_[i] = items_.back();
                items_.pop_back();
            } else {
                ++i;
            }
        }
        return picked;
    }

    // 部分拾取后把剩余堆叠写回（背包放不下时物品实体留在原地，vanilla 同）
    void reduce(std::uint32_t entity_id, const item::ItemStack& remaining) {
        std::lock_guard<std::mutex> lock{mutex_};
        for (auto& it : items_) {
            if (it.entity_id == entity_id) {
                it.stack = remaining;
                return;
            }
        }
    }

private:
    // EntityItem.searchForOtherItemsNearby → combineItems：盒 grow(0.5,0,0.5) 内的同类
    // 堆叠，大者吸收小者（count+count ≤ 64），吸收方继承 max(pickupDelay)/min(age)
    void combine_with_neighbors(std::size_t self_index, ItemTickResult& result) {
        DroppedItem& self = items_[self_index];
        for (std::size_t j = 0; j < items_.size(); ++j) {
            if (j == self_index) {
                continue;
            }
            DroppedItem& other = items_[j];
            if (other.stack.empty() || other.stack.id != self.stack.id ||
                other.stack.damage != self.stack.damage) {
                continue;
            }
            if (std::abs(other.x - self.x) >= 0.75 || std::abs(other.z - self.z) >= 0.75 ||
                std::abs(other.y - self.y) >= 0.25) {
                continue;  // grow(0.5, 0, 0.5) 的盒相交（严格小于）
            }
            if (other.pickup_delay == 32767 || self.pickup_delay == 32767) {
                continue;
            }
            DroppedItem* big = &self;
            DroppedItem* small = &other;
            if (small->stack.count > big->stack.count) {
                std::swap(big, small);
            }
            if (static_cast<int>(small->stack.count) + static_cast<int>(big->stack.count) >
                item::kMaxStack) {
                continue;
            }
            big->stack.count = static_cast<std::uint8_t>(big->stack.count + small->stack.count);
            big->pickup_delay = std::max(big->pickup_delay, small->pickup_delay);
            big->age = std::min(big->age, small->age);
            result.merged.push_back(
                ItemMerged{small->entity_id, big->entity_id, big->stack, big->x, big->y, big->z});
            small->stack = item::ItemStack::air();
            if (small == &self) {
                return;  // 本体被吸收：调用方负责移除
            }
        }
    }

    mutable std::mutex mutex_;
    std::vector<DroppedItem> items_;
};

}  // namespace cyane::net
