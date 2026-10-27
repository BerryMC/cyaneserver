#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "cyane/core/json_parse.hpp"
#include "cyane/item/item_stack.hpp"
#include "cyane/world/loot/loot_types.hpp"
#include "cyane/world/noise.hpp"

namespace cyane::world::loot {

// 物品名 → 物品 id 的解析器由调用方提供（服务端接入自己的注册表；不在此处内置任何名表）
using ItemResolver = std::function<std::optional<std::int16_t>(std::string_view name)>;

// 1.12.2 原版掉落条目（LootEntry / LootEntryItem / LootEntryTable / LootEntryEmpty）
class LootEntry {
public:
    enum class Type : std::uint8_t { Item, Table, Empty };

    Type type{Type::Empty};
    int weight{1};
    int quality{0};
    std::int16_t item_id{-1};
    std::string table_name;                      // type==Table 时生效
    std::vector<LootFunction> functions;         // type==Item 时生效
    std::vector<LootCondition> conditions;

    // LootEntry.getEffectiveWeight(luck)
    [[nodiscard]] int effective_weight(float luck) const noexcept {
        const int value = static_cast<int>(static_cast<float>(weight) + static_cast<float>(quality) * luck);
        return value < 0 ? 0 : value;
    }
};

// 1.12.2 原版掉落池（LootPool）
class LootPool {
public:
    std::vector<LootEntry> entries;
    std::vector<LootCondition> conditions;
    RandomValueRange rolls{1.0f};
    RandomValueRange bonus_rolls{0.0f};
    std::string name;

    void generate_loot(std::vector<item::ItemStack>& out, gen::JavaRandom& rand,
                       const LootContext& ctx, class LootTableManager& manager) const;

private:
    void create_loot_roll(std::vector<item::ItemStack>& out, gen::JavaRandom& rand,
                          const LootContext& ctx, class LootTableManager& manager) const;
};

// 1.12.2 原版掉落表（LootTable）
// 自引用保护：vanilla 用 context.addLootTable 去重，这里用 active_tables 集合等价实现。
class LootTable {
public:
    std::string name;
    std::vector<LootPool> pools;

    void generate_loot(std::vector<item::ItemStack>& out, gen::JavaRandom& rand,
                       const LootContext& ctx, class LootTableManager& manager) const;

    // vanilla LootTable.generateLootForPools
    void generate_loot_for_pools(std::vector<item::ItemStack>& out, gen::JavaRandom& rand,
                                 const LootContext& ctx, class LootTableManager& manager) const;

    // 从 loot table JSON 加载；解析失败返回 false（对齐 LootTable.Serializer）
    bool load(const core::JsonValue& json, const ItemResolver& resolver);
};

// 1.12.2 原版掉落表管理器（LootTableManager）
class LootTableManager {
public:
    explicit LootTableManager(ItemResolver resolver) : resolver_(std::move(resolver)) {}

    // 注册一张表；name 形如 "minecraft:chests/simple_dungeon"
    bool add_table(std::string name, const core::JsonValue& json) {
        LootTable table;
        table.name = name;
        if (!table.load(json, resolver_)) return false;
        tables_[std::move(name)] = std::move(table);
        return true;
    }

    // 从 JSON 文本直接注册
    bool add_table_json(std::string name, std::string_view json_text) {
        const auto parsed = core::JsonValue::parse(json_text);
        if (!parsed) return false;
        return add_table(std::move(name), *parsed);
    }

    [[nodiscard]] const LootTable* find(std::string_view name) const noexcept {
        const auto it = tables_.find(std::string{name});
        return it == tables_.end() ? nullptr : &it->second;
    }

    // 生成掉落；表不存在时返回空
    [[nodiscard]] std::vector<item::ItemStack> generate(std::string_view name, gen::JavaRandom& rand,
                                                        LootContext ctx) {
        std::vector<item::ItemStack> out;
        if (const auto* table = find(name)) {
            table->generate_loot(out, rand, ctx, *this);
        }
        return out;
    }

    [[nodiscard]] std::size_t size() const noexcept { return tables_.size(); }

    // 递归保护（vanilla LootContext.addLootTable / removeLootTable）
    [[nodiscard]] bool add_active_table(const std::string& name) { return active_tables_.insert(name).second; }
    void remove_active_table(const std::string& name) { active_tables_.erase(name); }

private:
    ItemResolver resolver_;
    std::unordered_map<std::string, LootTable> tables_;
    std::unordered_set<std::string> active_tables_;
};

// ---- LootPool 实现（依赖 LootTableManager，故定义在后面）----

inline void LootPool::create_loot_roll(std::vector<item::ItemStack>& out, gen::JavaRandom& rand,
                                       const LootContext& ctx, LootTableManager& manager) const {
    // 先筛出满足条件的条目并累加有效权重（vanilla 第一轮循环）
    std::vector<const LootEntry*> candidates;
    int total_weight = 0;
    for (const auto& entry : entries) {
        if (!test_all_conditions(entry.conditions, rand, ctx)) continue;
        const int weight = entry.effective_weight(ctx.luck);
        if (weight > 0) {
            candidates.push_back(&entry);
            total_weight += weight;
        }
    }
    if (total_weight == 0 || candidates.empty()) return;

    // 加权随机（vanilla 第二轮循环）
    int pick = rand.next_int(total_weight);
    for (const auto* entry : candidates) {
        pick -= entry->effective_weight(ctx.luck);
        if (pick < 0) {
            switch (entry->type) {
                case LootEntry::Type::Empty:
                    break;
                case LootEntry::Type::Table: {
                    if (const auto* nested = manager.find(entry->table_name)) {
                        if (manager.add_active_table(entry->table_name)) {
                            nested->generate_loot(out, rand, ctx, manager);
                            manager.remove_active_table(entry->table_name);
                        }
                    }
                    break;
                }
                case LootEntry::Type::Item: {
                    item::ItemStack stack{entry->item_id, 1, 0};
                    for (const auto& function : entry->functions) {
                        if (!test_all_conditions(function.conditions, rand, ctx)) continue;
                        function.apply(stack, rand, ctx);
                    }
                    if (!stack.empty()) out.push_back(stack);
                    break;
                }
            }
            return;
        }
    }
}

inline void LootPool::generate_loot(std::vector<item::ItemStack>& out, gen::JavaRandom& rand,
                                    const LootContext& ctx, LootTableManager& manager) const {
    if (!test_all_conditions(conditions, rand, ctx)) return;
    const int count = rolls.generate_int(rand) +
                      static_cast<int>(std::floor(bonus_rolls.generate_float(rand) * ctx.luck));
    for (int i = 0; i < count; ++i) {
        create_loot_roll(out, rand, ctx, manager);
    }
}

// ---- LootTable 实现 ----

inline void LootTable::generate_loot(std::vector<item::ItemStack>& out, gen::JavaRandom& rand,
                                     const LootContext& ctx, LootTableManager& manager) const {
    if (!manager.add_active_table(name)) return; // 自引用保护
    generate_loot_for_pools(out, rand, ctx, manager);
    manager.remove_active_table(name);
}

inline void LootTable::generate_loot_for_pools(std::vector<item::ItemStack>& out, gen::JavaRandom& rand,
                                               const LootContext& ctx, LootTableManager& manager) const {
    for (const auto& pool : pools) {
        pool.generate_loot(out, rand, ctx, manager);
    }
}

// ---- JSON 反序列化（对齐 vanilla Serializer.deserialize）----

namespace detail {

// RandomValueRange：对象 {min,max} 或裸数字
[[nodiscard]] inline RandomValueRange read_range(const core::JsonValue& value) {
    if (value.is_number()) {
        const auto single = static_cast<float>(value.as_number());
        return RandomValueRange{single};
    }
    const float lo = static_cast<float>(value.get_number("min", 0.0));
    const float hi = static_cast<float>(value.get_number("max", 0.0));
    return RandomValueRange{lo, hi};
}

[[nodiscard]] inline std::vector<LootCondition> read_conditions(const core::JsonValue& owner) {
    std::vector<LootCondition> out;
    const auto* array = owner.get_array("conditions");
    if (array == nullptr) return out;

    for (const auto& item : *array) {
        const auto tag = item.get_string("condition");
        LootCondition condition;
        if (tag == "random_chance") {
            condition.kind = LootCondition::Kind::RandomChance;
            condition.chance = static_cast<float>(item.get_number("chance", 0.0));
        } else if (tag == "random_chance_with_looting") {
            condition.kind = LootCondition::Kind::RandomChanceWithLooting;
            condition.chance = static_cast<float>(item.get_number("chance", 0.0));
            condition.looting_multiplier = static_cast<float>(item.get_number("looting_multiplier", 0.0));
        } else if (tag == "killed_by_player") {
            condition.kind = LootCondition::Kind::KilledByPlayer;
        } else {
            continue; // 未实现的条件下整个条目跳过（与原版“只解析已注册条件”一致）
        }
        out.push_back(condition);
    }
    return out;
}

[[nodiscard]] inline std::vector<LootFunction> read_functions(const core::JsonValue& owner) {
    std::vector<LootFunction> out;
    const auto* array = owner.get_array("functions");
    if (array == nullptr) return out;

    for (const auto& item : *array) {
        const auto tag = item.get_string("function");
        LootFunction function;
        function.conditions = read_conditions(item);
        if (tag == "set_count") {
            function.kind = LootFunction::Kind::SetCount;
            const auto* count = item.find("count");
            if (count == nullptr) continue;
            function.count = read_range(*count);
        } else if (tag == "set_data" || tag == "set_damage") {
            function.kind = LootFunction::Kind::SetData;
            const auto* data = item.find("data");
            if (data != nullptr) {
                function.count = RandomValueRange{static_cast<float>(data->as_number())};
            } else if (const auto* damage = item.find("damage"); damage != nullptr) {
                function.count = read_range(*damage);
            }
        } else if (tag == "looting_enchant") {
            function.kind = LootFunction::Kind::LootingEnchant;
            if (const auto* count = item.find("count"); count != nullptr) {
                function.count = read_range(*count);
            }
            function.limit = static_cast<int>(item.get_int("limit", 0));
        } else {
            continue;
        }
        out.push_back(function);
    }
    return out;
}

}  // namespace detail

inline bool LootTable::load(const core::JsonValue& json, const ItemResolver& resolver) {
    pools.clear();
    const auto* pool_array = json.get_array("pools");
    if (pool_array == nullptr) return false;

    for (const auto& pool_json : *pool_array) {
        LootPool pool;
        pool.name = std::string{pool_json.get_string("name", "")};
        pool.conditions = detail::read_conditions(pool_json);

        if (const auto* rolls = pool_json.find("rolls"); rolls != nullptr) {
            pool.rolls = detail::read_range(*rolls);
        }
        if (const auto* bonus = pool_json.find("bonus_rolls"); bonus != nullptr) {
            pool.bonus_rolls = detail::read_range(*bonus);
        }

        const auto* entry_array = pool_json.get_array("entries");
        if (entry_array == nullptr) continue;

        for (const auto& entry_json : *entry_array) {
            LootEntry entry;
            entry.weight = static_cast<int>(entry_json.get_int("weight", 1));
            entry.quality = static_cast<int>(entry_json.get_int("quality", 0));
            entry.conditions = detail::read_conditions(entry_json);

            const auto type_tag = entry_json.get_string("type", "item");
            if (type_tag == "empty") {
                entry.type = LootEntry::Type::Empty;
            } else if (type_tag == "loot_table") {
                entry.type = LootEntry::Type::Table;
                entry.table_name = std::string{entry_json.get_string("name", "")};
                if (entry.table_name.empty()) continue;
            } else if (type_tag == "item") {
                entry.type = LootEntry::Type::Item;
                const auto item_name = entry_json.get_string("name", "");
                const auto id = resolver ? resolver(item_name) : std::nullopt;
                if (!id) continue; // 未注册物品：跳过（原版会抛异常，服务端更稳妥是跳过）
                entry.item_id = *id;
                entry.functions = detail::read_functions(entry_json);
            } else {
                continue;
            }
            pool.entries.push_back(std::move(entry));
        }

        if (!pool.entries.empty()) pools.push_back(std::move(pool));
    }
    return true;
}

// 1.12.2 内置掉落表名（LootTableList）
namespace table_names {
inline constexpr std::string_view kEmpty = "minecraft:empty";
inline constexpr std::string_view kChestsSimpleDungeon = "minecraft:chests/simple_dungeon";
inline constexpr std::string_view kChestsAbandonedMineshaft = "minecraft:chests/abandoned_mineshaft";
inline constexpr std::string_view kChestsVillageBlacksmith = "minecraft:chests/village_blacksmith";
inline constexpr std::string_view kChestsStrongholdCorridor = "minecraft:chests/stronghold_corridor";
inline constexpr std::string_view kChestsStrongholdCrossing = "minecraft:chests/stronghold_crossing";
inline constexpr std::string_view kChestsStrongholdLibrary = "minecraft:chests/stronghold_library";
inline constexpr std::string_view kChestsNetherBridge = "minecraft:chests/nether_bridge";
inline constexpr std::string_view kChestsDesertPyramid = "minecraft:chests/desert_pyramid";
inline constexpr std::string_view kChestsJungleTemple = "minecraft:chests/jungle_temple";
inline constexpr std::string_view kChestsJungleTempleDispenser = "minecraft:chests/jungle_temple_dispenser";
inline constexpr std::string_view kChestsIglooChest = "minecraft:chests/igloo_chest";
inline constexpr std::string_view kChestsEndCityTreasure = "minecraft:chests/end_city_treasure";
inline constexpr std::string_view kChestsWoodlandMansion = "minecraft:chests/woodland_mansion";
inline constexpr std::string_view kSpawnBonusChest = "minecraft:chests/spawn_bonus_chest";
}  // namespace table_names

}  // namespace cyane::world::loot
