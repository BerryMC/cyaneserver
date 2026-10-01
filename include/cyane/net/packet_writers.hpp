#pragma once

// 实体类 clientbound 包的载荷编码（不含 packet id 与长度前缀）。
// 这些函数是协议格式的唯一书写点——对应黄金向量测试见 tests/test_packets.cpp；
// 修改任何字段顺序/宽度必须同步更新测试并对照 wiki.vg 1.12.2 事实。
// entityId 一律 varint（EntityStatus 除外，协议规定 int）。

#include <cstdint>
#include <span>

#include "cyane/core/bytes.hpp"

namespace cyane::net::writers {

// 角度 float(度) → 1/256 圈的字节角
[[nodiscard]] inline std::uint8_t angle_byte(float degrees) noexcept {
    return static_cast<std::uint8_t>(static_cast<int>(degrees * 256.0f / 360.0f) & 0xFF);
}

// Animation (0x06)：varint entityId | byte hand
inline void write_animation(ByteWriter& out, std::uint32_t entity_id, std::uint8_t hand) {
    out.varint(static_cast<std::int32_t>(entity_id));
    out.u8(hand);
}

// BlockBreakAnimation (0x08)：varint entityId | position | byte progress
inline void write_block_break_animation(ByteWriter& out, std::uint32_t entity_id, std::int32_t x,
                                        std::int32_t y, std::int32_t z, std::uint8_t stage) {
    out.varint(static_cast<std::int32_t>(entity_id));
    out.position(x, y, z);
    out.u8(stage);
}

// HeldItemChange (0x3A)：仅 1 字节热区栏槽位（0..8）
inline void write_held_item_change(ByteWriter& out, std::uint8_t slot) {
    out.u8(slot);
}

// CollectItem (0x4B)：varint collected | varint collector | varint count
inline void write_collect_item(ByteWriter& out, std::uint32_t collected, std::uint32_t collector,
                               std::uint8_t count) {
    out.varint(static_cast<std::int32_t>(collected));
    out.varint(static_cast<std::int32_t>(collector));
    out.varint(static_cast<std::int32_t>(count));
}

// EntityStatus (0x19)：int entityId | byte status（此包协议确为 int）
inline void write_entity_status(ByteWriter& out, std::uint32_t entity_id, std::uint8_t status) {
    out.i32(static_cast<std::int32_t>(entity_id));
    out.u8(status);
}

// DestroyEntities (0x32)：varint count | varint[] ids
inline void write_destroy_entities(ByteWriter& out, std::span<const std::uint32_t> ids) {
    out.varint(static_cast<std::int32_t>(ids.size()));
    for (const auto id : ids) {
        out.varint(static_cast<std::int32_t>(id));
    }
}

// EntityTeleport (0x4C)：varint entityId | double x/y/z | byte yaw | byte pitch | bool onGround
inline void write_entity_teleport(ByteWriter& out, std::uint32_t entity_id, double x, double y,
                                  double z, float yaw, float pitch, bool on_ground) {
    out.varint(static_cast<std::int32_t>(entity_id));
    out.f64(x);
    out.f64(y);
    out.f64(z);
    out.u8(angle_byte(yaw));
    out.u8(angle_byte(pitch));
    out.boolean(on_ground);
}

// EntityHeadLook (0x36)：varint entityId | byte headYaw
inline void write_entity_head_look(ByteWriter& out, std::uint32_t entity_id, float head_yaw) {
    out.varint(static_cast<std::int32_t>(entity_id));
    out.u8(angle_byte(head_yaw));
}

} // namespace cyane::net::writers
