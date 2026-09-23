#pragma once

#include <array>
#include <cstdint>
#include <string_view>

#include "cyane/core/bytes.hpp"
#include "cyane/proto/packet_ids.hpp"
#include "cyane/world/chunk.hpp"

// Connection 各翻译单元共享的编码 helper。仅供 src/net/connection*.cpp 内部使用。
namespace cyane::net::detail {

// 角度 float(度) → 1/256 圈的字节角
[[nodiscard]] inline std::uint8_t to_angle_byte(float degrees) noexcept {
    return static_cast<std::uint8_t>(static_cast<int>(degrees * 256.0f / 360.0f) & 0xFF);
}

// 区块坐标打包成 64 位键，用于集合去重
[[nodiscard]] inline std::int64_t chunk_key(world::ChunkPos pos) noexcept {
    return (static_cast<std::int64_t>(pos.x) << 32) | (static_cast<std::uint32_t>(pos.z));
}

// 方块世界坐标打包成 64 位键（26 位 x | 12 位 y | 26 位 z），用于容器索引
[[nodiscard]] inline std::int64_t block_key(std::int32_t x, std::int32_t y, std::int32_t z) noexcept {
    return (static_cast<std::int64_t>(x & 0x3FFFFFF) << 38) |
           (static_cast<std::int64_t>(y & 0xFFF) << 26) |
           static_cast<std::int64_t>(z & 0x3FFFFFF);
}

inline void write_player_info_add(ByteWriter& out, const std::array<std::uint8_t, 16>& uuid,
                                  std::string_view name) {
    out.varint(proto::play_cb::kPlayerInfoAddPlayer);
    out.varint(1);
    out.bytes(ByteSpan{reinterpret_cast<const std::byte*>(uuid.data()), uuid.size()});
    out.string(name);
    out.varint(0);   // properties
    out.varint(0);   // gameMode
    out.varint(20);  // ping
    out.boolean(false);
}

// NamedEntitySpawn (0x05)：varint id | uuid(16) | double x/y/z | byte yaw | byte pitch | metadata(0xff 终止)
inline void write_named_spawn(ByteWriter& out, std::uint32_t entity_id,
                              const std::array<std::uint8_t, 16>& uuid, double x, double y, double z,
                              float yaw, float pitch) {
    out.varint(static_cast<std::int32_t>(entity_id));
    out.bytes(ByteSpan{reinterpret_cast<const std::byte*>(uuid.data()), uuid.size()});
    out.f64(x);
    out.f64(y);
    out.f64(z);
    out.u8(to_angle_byte(yaw));
    out.u8(to_angle_byte(pitch));
    out.u8(0xFF);
}

}
