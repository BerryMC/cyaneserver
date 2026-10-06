#pragma once

// 实体类 clientbound 包的载荷编码（不含 packet id 与长度前缀）。
// 这些函数是协议格式的唯一书写点——对应黄金向量测试见 tests/test_packets.cpp；
// 修改任何字段顺序/宽度必须同步更新测试并对照 wiki.vg 1.12.2 事实。
// entityId 一律 varint（EntityStatus 除外，协议规定 int）。

#include <algorithm>
#include <array>
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

// SpawnMob (0x03) / EntityLiving：varint id | uuid(16) | varint type | double x/y/z
//   | byte yaw/pitch/head | short vx/vy/vz | metadata | 0xFF
// uuid 由实体 id 合成（本服务端不维护实体 UUID 表，客户端仅用它去重）
// metadata：出生必须带全客户端会用的字段——客户端只在出生包里注册 watcher 条目，
// 之后对未注册 index 的增量 EntityMetadata 会被忽略（苦力怕白闪失效的根因）。
// 格式：index u8 | 序列化器 varint | 值 | …（EntityMetadata 的条目格式）
inline void write_spawn_mob(ByteWriter& out, std::uint32_t entity_id, std::int32_t type, double x,
                            double y, double z, float yaw, std::span<const std::byte> metadata) {
    out.varint(static_cast<std::int32_t>(entity_id));
    std::array<std::uint8_t, 16> uuid{};
    uuid[15] = static_cast<std::uint8_t>(entity_id & 0xFF);
    uuid[14] = static_cast<std::uint8_t>((entity_id >> 8) & 0xFF);
    out.bytes(ByteSpan{reinterpret_cast<const std::byte*>(uuid.data()), uuid.size()});
    out.varint(type);
    out.f64(x);
    out.f64(y);
    out.f64(z);
    out.u8(angle_byte(yaw));
    out.u8(0);           // pitch
    out.u8(angle_byte(yaw));  // head pitch
    out.i16(0);
    out.i16(0);
    out.i16(0);
    out.bytes(ByteSpan{reinterpret_cast<const std::byte*>(metadata.data()), metadata.size()});
    out.u8(0xFF);
}

// SpawnObject (0x00)：varint id | uuid(16) | byte type | f64 x/y/z | byte pitch/yaw
//   | i32 data | short vx/vy/vz（速度单位 1/8000，data≠0 时客户端才读速度）
inline void write_spawn_object(ByteWriter& out, std::uint32_t entity_id, std::uint8_t object_type,
                               double x, double y, double z, float yaw, float pitch,
                               std::int32_t data, double vx, double vy, double vz) {
    out.varint(static_cast<std::int32_t>(entity_id));
    std::array<std::uint8_t, 16> uuid{};
    uuid[15] = static_cast<std::uint8_t>(entity_id & 0xFF);
    uuid[14] = static_cast<std::uint8_t>((entity_id >> 8) & 0xFF);
    out.bytes(ByteSpan{reinterpret_cast<const std::byte*>(uuid.data()), uuid.size()});
    out.u8(object_type);
    out.f64(x);
    out.f64(y);
    out.f64(z);
    out.u8(angle_byte(pitch));
    out.u8(angle_byte(yaw));
    out.i32(data);
    auto clamp_i16 = [](double v) {
        return static_cast<std::int16_t>(std::clamp(v, -3.9, 3.9) * 8000.0);
    };
    out.i16(clamp_i16(vx));
    out.i16(clamp_i16(vy));
    out.i16(clamp_i16(vz));
}

// EntityVelocity (0x3E)：varint id | short vx/vy/vz（单位 = 1/8000 格/tick）
inline void write_entity_velocity(ByteWriter& out, std::uint32_t entity_id, double vx, double vy,
                                  double vz) {
    constexpr double kScale = 8000.0;
    auto clamp_i16 = [](double v) {
        const double clamped = std::clamp(v, -3.9, 3.9) * kScale;
        return static_cast<std::int16_t>(clamped);
    };
    out.varint(static_cast<std::int32_t>(entity_id));
    out.i16(clamp_i16(vx));
    out.i16(clamp_i16(vy));
    out.i16(clamp_i16(vz));
}

// EntityStatus (0x19)：int entityId | byte status（此包协议确为 int）
inline void write_entity_status(ByteWriter& out, std::uint32_t entity_id, std::uint8_t status) {
    out.i32(static_cast<std::int32_t>(entity_id));
    out.u8(status);
}

// Named Sound Effect (0x49)：varint soundId | varint category | int (x+0.5)*8 ... | f32 vol | f32 pitch
// 1.12.2 的 soundId 是 SoundEffect 注册表 id，不是名字。
inline void write_named_sound(ByteWriter& out, std::int32_t sound_id, std::int32_t category,
                              std::int32_t x, std::int32_t y, std::int32_t z, float volume,
                              float pitch) {
    out.varint(sound_id);
    out.varint(category);
    out.i32(x * 8 + 4);
    out.i32(y * 8 + 4);
    out.i32(z * 8 + 4);
    out.f32(volume);
    out.f32(pitch);
}

// Explosion (0x1C)：位置 f32×3（1.12.2 字段虽声明 double 但按 float 写出！）、半径 f32、
// 记录数 i32、每条记录 byte×3（受影响方块相对爆心偏移）、玩家动量 f32×3。
// 位置写成 f64 会让客户端把半径/记录数读串位——记录数为垃圾值时客户端按其
// 分配内存直接 OOM（线上事故 R-022）。
inline void write_explosion(ByteWriter& out, double x, double y, double z, float power,
                            std::span<const std::array<std::int8_t, 3>> records) {
    out.f32(static_cast<float>(x));
    out.f32(static_cast<float>(y));
    out.f32(static_cast<float>(z));
    out.f32(power);
    out.i32(static_cast<std::int32_t>(records.size()));
    for (const auto& record : records) {
        out.u8(static_cast<std::uint8_t>(record[0]));
        out.u8(static_cast<std::uint8_t>(record[1]));
        out.u8(static_cast<std::uint8_t>(record[2]));
    }
    out.f32(0.0f);  // 玩家动量（击退经 EntityVelocity 单独下发）
    out.f32(0.0f);
    out.f32(0.0f);
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

// SpawnExperienceOrb (0x01): varint entityId | double x | double y | double z | short count
inline void write_spawn_experience_orb(ByteWriter& out, std::uint32_t entity_id, double x, double y, double z, std::int16_t count) {
    out.varint(static_cast<std::int32_t>(entity_id));
    out.f64(x);
    out.f64(y);
    out.f64(z);
    out.i16(count);
}

// SetExperience (0x40): float experienceBar | varint level | varint totalExperience
inline void write_set_experience(ByteWriter& out, float experience_bar, std::int32_t level, std::int32_t total_experience) {
    out.f32(experience_bar);
    out.varint(level);
    out.varint(total_experience);
}

} // namespace cyane::net::writers
