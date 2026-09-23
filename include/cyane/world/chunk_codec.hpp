#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "cyane/core/bytes.hpp"
#include "cyane/world/chunk.hpp"

namespace cyane::world {

namespace detail {

inline constexpr std::size_t kBiomeBytes = 256;
inline constexpr std::uint8_t kBiomePlains = 1;

// 至少 4 位；调色板索引位宽 = 表示 (paletteSize-1) 所需位数
[[nodiscard]] inline std::uint8_t bits_for_palette(std::size_t palette_size) noexcept {
    std::uint8_t bits = 4;
    while ((static_cast<std::size_t>(1) << bits) < palette_size) {
        ++bits;
    }
    return bits;
}

// 复刻 DataBits.a：把 4096 个 bitsPerBlock 位的索引打进 long[]，允许跨 long 边界
[[nodiscard]] inline std::vector<std::uint64_t> pack_indices(const std::vector<std::uint16_t>& indices,
                                                             std::uint8_t bits) {
    const std::size_t total_bits = indices.size() * bits;
    const std::size_t long_count = (total_bits + 63) / 64;
    std::vector<std::uint64_t> data(long_count, 0);
    const std::uint64_t mask = (bits >= 64) ? ~0ULL : ((1ULL << bits) - 1ULL);

    for (std::size_t i = 0; i < indices.size(); ++i) {
        const std::uint64_t value = static_cast<std::uint64_t>(indices[i]) & mask;
        const std::size_t bit_index = i * bits;
        const std::size_t start_long = bit_index / 64;
        const std::size_t end_long = ((i + 1) * bits - 1) / 64;
        const std::size_t offset = bit_index % 64;

        data[start_long] |= value << offset;
        if (start_long != end_long) {
            data[end_long] |= value >> (64 - offset);
        }
    }
    return data;
}

// 序列化单个 section（对应 DataPaletteBlock.b + 光照数组）
inline void write_section(ByteWriter& out, const Section& section, bool overworld) {
    std::vector<std::uint16_t> palette;
    palette.reserve(16);
    std::array<int, 1 << 13> lookup{};  // 全局状态 id → 调色板索引，-1 未入表
    lookup.fill(-1);

    std::vector<std::uint16_t> indices(kSectionBlockCount, 0);
    for (std::size_t i = 0; i < kSectionBlockCount; ++i) {
        const std::uint16_t state = section.state(i);
        int idx = lookup[state];
        if (idx < 0) {
            idx = static_cast<int>(palette.size());
            lookup[state] = idx;
            palette.push_back(state);
        }
        indices[i] = static_cast<std::uint16_t>(idx);
    }

    const std::uint8_t bits = bits_for_palette(palette.size());
    out.u8(bits);

    // 线性调色板：varint count | 每项 varint(全局状态 id)
    out.varint(static_cast<std::int32_t>(palette.size()));
    for (const std::uint16_t state : palette) {
        out.varint(static_cast<std::int32_t>(state));
    }

    // 打包 long[]：varint(长度) | 每个 long 大端
    const auto packed = pack_indices(indices, bits);
    out.varint(static_cast<std::int32_t>(packed.size()));
    for (const std::uint64_t word : packed) {
        out.i64(static_cast<std::int64_t>(word));
    }

    // 方块光照 2048 字节（全 0）
    static const std::array<std::byte, kLightArrayBytes> kZeroLight{};
    out.bytes(ByteSpan{kZeroLight.data(), kZeroLight.size()});

    // 主世界额外写天空光 2048 字节（全亮 0xFF）
    if (overworld) {
        std::array<std::byte, kLightArrayBytes> sky{};
        sky.fill(static_cast<std::byte>(0xFF));
        out.bytes(ByteSpan{sky.data(), sky.size()});
    }
}

}

// 把整块序列化为 Chunk Data(0x20) 字段（不含帧头）：
//   int chunkX | int chunkZ | bool groundUp=true | varint primaryBitMask
//   | varint dataLength | byte[] data | varint blockEntityCount(=0)
inline void write_full_chunk(ByteWriter& out, const Chunk& chunk) {
    constexpr bool overworld = true;

    std::int32_t primary_bit_mask = 0;
    for (int y = 0; y < kSectionCount; ++y) {
        const Section* sec = chunk.section(static_cast<std::size_t>(y));
        if (sec != nullptr && !sec->empty()) {
            primary_bit_mask |= (1 << y);
        }
    }

    ByteWriter data;
    for (int y = 0; y < kSectionCount; ++y) {
        if ((primary_bit_mask & (1 << y)) == 0) {
            continue;
        }
        detail::write_section(data, *chunk.section(static_cast<std::size_t>(y)), overworld);
    }
    std::array<std::byte, detail::kBiomeBytes> biomes{};
    biomes.fill(static_cast<std::byte>(detail::kBiomePlains));
    data.bytes(ByteSpan{biomes.data(), biomes.size()});

    out.i32(chunk.pos().x);
    out.i32(chunk.pos().z);
    out.boolean(true);  // ground-up continuous（full chunk）
    out.varint(primary_bit_mask);
    out.varint(static_cast<std::int32_t>(data.size()));
    out.bytes(data.data());
    out.varint(0);  // block entity count
}

// 超平坦出生区块：y0 基岩、y1..y2 泥土、y3 草方块，其余空气
[[nodiscard]] inline Chunk make_flat_chunk(ChunkPos pos) {
    Chunk chunk{pos};
    const std::int32_t base_x = pos.world_x();
    const std::int32_t base_z = pos.world_z();
    for (int lx = 0; lx < kChunkSizeX; ++lx) {
        for (int lz = 0; lz < kChunkSizeZ; ++lz) {
            const std::int32_t wx = base_x + lx;
            const std::int32_t wz = base_z + lz;
            chunk.set_block_state(wx, 0, wz, kStateBedrock);
            chunk.set_block_state(wx, 1, wz, kStateDirt);
            chunk.set_block_state(wx, 2, wz, kStateDirt);
            chunk.set_block_state(wx, 3, wz, kStateGrass);
        }
    }
    return chunk;
}

}
