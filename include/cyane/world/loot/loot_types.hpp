#pragma once

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>
#include "cyane/item/item_stack.hpp"
#include "cyane/world/noise.hpp"

namespace cyane::world::loot {

// 1.12.2 原版随机值区间（RandomValueRange 逐行转写）
// generate_int = MathHelper.getInt(rand, floor(min), floor(max))
// generate_float = MathHelper.nextFloat(rand, min, max)
struct RandomValueRange {
    float min{0.0f};
    float max{0.0f};

    constexpr RandomValueRange() noexcept = default;
    constexpr explicit RandomValueRange(float value) noexcept : min(value), max(value) {}
    constexpr RandomValueRange(float lo, float hi) noexcept : min(lo), max(hi) {}

    [[nodiscard]] int generate_int(gen::JavaRandom& rand) const noexcept {
        const int lo = static_cast<int>(std::floor(min));
        const int hi = static_cast<int>(std::floor(max));
        return lo >= hi ? lo : rand.next_int(hi - lo + 1) + lo;
    }

    [[nodiscard]] float generate_float(gen::JavaRandom& rand) const noexcept {
        return rand.next_float() * (max - min) + min;
    }

    [[nodiscard]] bool is_in_range(int value) const noexcept {
        return static_cast<float>(value) <= max && static_cast<float>(value) >= min;
    }
};

// 1.12.2 原版掉落上下文（LootContext 的服务端可用子集）
// 实体/世界对象在 C++ 侧按值语义传递，这里只保留掉落判定真正用到的字段。
struct LootContext {
    float luck{0.0f};
    int looting_modifier{0};   // EntityLivingBase.getLootingModifier
    bool killer_is_player{false};
    bool has_living_killer{false};
};

// 1.12.2 原版掉落条件（LootCondition 的已实现子类，按 tag 分派）
class LootCondition {
public:
    enum class Kind : std::uint8_t {
        RandomChance,
        RandomChanceWithLooting,
        KilledByPlayer,
    };

    Kind kind{Kind::RandomChance};
    float chance{0.0f};
    float looting_multiplier{0.0f};

    [[nodiscard]] bool test(gen::JavaRandom& rand, const LootContext& ctx) const noexcept {
        switch (kind) {
            case Kind::RandomChance:
                return rand.next_float() < chance;
            case Kind::RandomChanceWithLooting:
                return rand.next_float() < chance + static_cast<float>(ctx.looting_modifier) * looting_multiplier;
            case Kind::KilledByPlayer:
                return ctx.killer_is_player;
        }
        return false;
    }
};

// 1.12.2 原版掉落函数（LootFunction 的已实现子类）
class LootFunction {
public:
    enum class Kind : std::uint8_t {
        SetCount,
        SetData,        // set_data / set_damage 同语义：写 damage
        LootingEnchant, // looting_enchant
        Smelt,          // furnace_smelt：掉落物替换为熔炼产物
    };

    Kind kind{Kind::SetCount};
    RandomValueRange count{1.0f};
    int limit{0};
    std::int16_t smelt_result{-1};  // Smelt 用：加载期由熔炉配方解析出的产物 id
    std::vector<LootCondition> conditions;

    // apply：对齐 vanilla 各 LootFunction.apply
    void apply(item::ItemStack& stack, gen::JavaRandom& rand, const LootContext& ctx) const noexcept {
        switch (kind) {
            case Kind::SetCount: {
                const int value = count.generate_int(rand);
                stack.count = static_cast<std::uint8_t>(value < 0 ? 0 : (value > 64 ? 64 : value));
                break;
            }
            case Kind::SetData:
                stack.damage = static_cast<std::int16_t>(count.generate_int(rand));
                break;
            case Kind::Smelt: {
                // vanilla Smelt.apply：换成熔炼产物并保留数量；无配方则原样返回
                if (smelt_result >= 0) {
                    stack.id = smelt_result;
                    stack.damage = 0;
                }
                break;
            }
            case Kind::LootingEnchant: {
                if (!ctx.has_living_killer) break;
                if (ctx.looting_modifier == 0) break;
                const float amount = static_cast<float>(ctx.looting_modifier) * count.generate_float(rand);
                const int grown = static_cast<int>(stack.count) + static_cast<int>(std::lround(amount));
                stack.count = static_cast<std::uint8_t>(grown < 0 ? 0 : (grown > 255 ? 255 : grown));
                if (limit != 0 && stack.count > limit) {
                    stack.count = static_cast<std::uint8_t>(limit);
                }
                break;
            }
        }
    }
};

[[nodiscard]] inline bool test_all_conditions(const std::vector<LootCondition>& conditions,
                                              gen::JavaRandom& rand, const LootContext& ctx) noexcept {
    for (const auto& condition : conditions) {
        if (!condition.test(rand, ctx)) return false;
    }
    return true;
}

}  // namespace cyane::world::loot
