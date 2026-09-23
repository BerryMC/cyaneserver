#pragma once

#include <cstdint>
#include <optional>

#include "cyane/core/bytes.hpp"

namespace cyane::item {

// 1.12.2 网络 slot：short id | byte count | short damage | NBT。id=-1 表示空槽。
// 本阶段不解析 NBT，读到时跳过 TAG_End(0)，写出恒为无 NBT(0)。
struct ItemStack {
    std::int16_t id{-1};
    std::uint8_t count{0};
    std::int16_t damage{0};

    [[nodiscard]] bool empty() const noexcept { return id < 0 || count == 0; }

    [[nodiscard]] static ItemStack air() noexcept { return {}; }
};

inline void write_slot(ByteWriter& out, const ItemStack& item) {
    if (item.empty()) {
        out.i16(-1);
        return;
    }
    out.i16(item.id);
    out.u8(item.count);
    out.i16(item.damage);
    out.u8(0);  // NBT：TAG_End，无标签
}

// 从网络读取一个 slot。NBT 非 TAG_End 时本阶段无法安全跳过，返回 nullopt 交由上层断连。
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
    auto nbt_tag = in.u8();
    if (!count || !damage || !nbt_tag) {
        return std::nullopt;
    }
    if (*nbt_tag != 0) {
        return std::nullopt;  // 携带 NBT 的物品暂不支持
    }
    return ItemStack{*id, *count, *damage};
}

}
