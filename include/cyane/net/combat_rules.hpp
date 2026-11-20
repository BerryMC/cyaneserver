#pragma once

namespace cyane::net {

// EntityPlayer.attackTargetEntityWithCurrentItem 里的三个判定，抽成纯函数便于单测。
// 原版（1.12.2）：
//   boolean flag  = f2 > 0.9F;                       // 蓄力满
//   flag1 = isSprinting() && flag;                   // 疾跑击退
//   flag2 = flag && fallDistance > 0 && !onGround && !isOnLadder() && !isInWater()
//           && !blindness && !isRiding() && target instanceof EntityLivingBase;
//   flag2 = flag2 && !isSprinting();                 // → 伤害 ×1.5
//   flag3 = flag && !flag2 && !flag1 && onGround && d0 < getAIMoveSpeed()
//           && heldItem instanceof ItemSword;        // → 横扫
struct AttackConditions {
    bool charged{false};        // f2 > 0.9
    float fall_distance{0.0f};  // Entity.fallDistance
    bool on_ground{true};
    bool sprinting{false};
    bool in_water{false};
    bool holding_sword{false};
    bool walked_toward{false};  // d0 < getAIMoveSpeed()（仅横扫用）
};

struct AttackOutcome {
    bool crit{false};
    bool sprint_knockback{false};
    bool sweeping{false};
    float knockback{0.4f};
};

[[nodiscard]] constexpr AttackOutcome resolve_attack(const AttackConditions& c) noexcept {
    AttackOutcome out;
    out.sprint_knockback = c.sprinting && c.charged;
    // 原版 flag2 末尾再与 !isSprinting() 相与
    out.crit = c.charged && c.fall_distance > 0.0f && !c.on_ground && !c.in_water &&
               !c.sprinting;
    out.sweeping = c.charged && !out.crit && !out.sprint_knockback && c.on_ground &&
                   c.walked_toward && c.holding_sword;
    // 疾跑击退把 knockbackModifier 至少提到 1 → knockBack(this, i*0.5F, ...) = 0.5
    out.knockback = out.sprint_knockback ? 0.5f : 0.4f;
    return out;
}

}  // namespace cyane::net
