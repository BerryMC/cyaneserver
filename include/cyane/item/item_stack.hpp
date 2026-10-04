#pragma once

#include <cstdint>
#include <optional>

#include "cyane/core/bytes.hpp"
#include "cyane/world/mob_types.hpp"
#include "cyane/world/nbt.hpp"

namespace cyane::item {

// 1.12.2 网络 slot：short id | byte count | short damage | NBT（TAG_End=无，否则为完整 NBT 树）。
// ItemStack 不保存 NBT：读入时解析并跳过；刷怪蛋的 EntityTag.id 翻译成 damage（1.12.2 的
// 创造栏蛋 damage=0、实体类型只在 NBT 里），写出时按 damage 反向重建 EntityTag。
struct ItemStack {
    std::int16_t id{-1};
    std::uint8_t count{0};
    std::int16_t damage{0};

    [[nodiscard]] bool empty() const noexcept { return id < 0 || count == 0; }

    [[nodiscard]] static ItemStack air() noexcept { return {}; }

    // 两个堆叠可否合并：非空、同 id 同 damage
    [[nodiscard]] bool stacks_with(const ItemStack& other) const noexcept {
        return !empty() && !other.empty() && id == other.id && damage == other.damage;
    }

    // NMS ItemStack.matches 语义：按物品类型（id+damage）比较，不含数量——游标校验用
    [[nodiscard]] bool matches_type(const ItemStack& other) const noexcept {
        return id == other.id && damage == other.damage;
    }
};

// 1.12.2 绝大多数物品堆叠上限 64（工具/盔甲等为 1，本阶段统一按 64 近似）
inline constexpr std::uint8_t kMaxStack = 64;

inline constexpr std::int16_t kSpawnEggItem = 383;

// 刷怪蛋写出：按 damage 重建 EntityTag NBT（客户端渲染蛋壳斑点与放置语义都依赖它）
inline void write_spawn_egg_nbt(ByteWriter& out, std::int16_t damage) {
    const auto species = world::mob_type(damage);
    if (!species) {
        out.u8(0);
        return;
    }
    // NBT 树：TAG_Compound("") { EntityTag: Compound { id: String } }。
    // 条目顺序 = tag 字节 → 名字 → 载荷。
    const std::string_view nbt_id{species->nbt_id};
    const auto put = [&out](std::string_view text) {
        out.u8(static_cast<std::uint8_t>(text.size() >> 8));
        out.u8(static_cast<std::uint8_t>(text.size() & 0xFF));
        out.bytes(ByteSpan{reinterpret_cast<const std::byte*>(text.data()), text.size()});
    };
    out.u8(static_cast<std::uint8_t>(world::nbt::Tag::compound));
    put("");                        // 根名（空）
    out.u8(static_cast<std::uint8_t>(world::nbt::Tag::compound));
    put("EntityTag");               // 条目：tag + 名字
    out.u8(static_cast<std::uint8_t>(world::nbt::Tag::string));
    put("id");                      // 条目：tag + 名字
    put(nbt_id);                    // 字符串载荷：u16 长度 + 字节
    out.u8(0);                      // EntityTag 结束
    out.u8(0);                      // 根结束
}

inline void write_slot(ByteWriter& out, const ItemStack& item) {
    if (item.empty()) {
        out.i16(-1);
        return;
    }
    out.i16(item.id);
    out.u8(item.count);
    out.i16(item.damage);
    if (item.id == kSpawnEggItem) {
        write_spawn_egg_nbt(out, item.damage);
    } else {
        out.u8(0);  // NBT：TAG_End，无标签
    }
}

// 从网络读取一个 slot。带 NBT 时解析并跳过（EntityTag 翻译成 damage）；解析失败返回
// nullopt 交由上层断连（NBT 声称存在却损坏 = 流已不可信）。
[[nodiscard]] inline std::optional<ItemStack> read_slot(ByteReader& in) {
    auto id = in.i16();
    if (!id) {
        return std::nullopt;
    }
    if (*id < 0) {
        return ItemStack::air();
    }
    auto count = in.u8();
    auto damage = in.i16();
    if (!count || !damage || in.remaining() < 1) {
        return std::nullopt;
    }
    std::int16_t effective_damage = *damage;
    // tag 字节只窥视不消耗：解析器需要完整的 NBT 树（含根 tag）
    const auto nbt_tag = std::to_integer<std::uint8_t>(in.rest().front());
    if (nbt_tag == 0) {
        in.skip(1);  // TAG_End：无 NBT
    } else {
        const auto parsed = world::nbt::parse_with_size(in.rest());
        if (!parsed) {
            return std::nullopt;
        }
        in.skip(parsed->second);
        // 刷怪蛋：1.12.2 实体类型在 EntityTag.id（damage 常为 0），翻译成 damage 供下游使用
        if (*id == kSpawnEggItem) {
            const auto* entity_tag = parsed->first.find("EntityTag");
            if (entity_tag != nullptr) {
                const auto* egg_id = entity_tag->find("id");
                if (egg_id != nullptr) {
                    if (const auto type = world::mob_type_from_nbt(*egg_id->text())) {
                        effective_damage = static_cast<std::int16_t>(*type);
                    }
                }
            }
        }
    }
    return ItemStack{*id, *count, effective_damage};
}

}
