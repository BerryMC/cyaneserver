#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "cyane/core/proxy_byte_reader.hpp"
#include "cyane/proto/packet_ids.hpp"
#include "cyane/world/chunk.hpp"

namespace cyane::proto {

// 将原版的ChunkSection数据写入ByteWriter（用于ChunkData包的data字段）
void write_chunk_section_data(ByteWriter& out, const net::minecraft::server::v1_12_R1::ChunkSection& section, bool& biome_exists);

// 从ByteSpan中解析原版的ChunkSection数据（用于ChunkData包的data字段）
std::unique_ptr<net::minecraft::server::v1_12_R1::ChunkSection> read_chunk_section_data(const ByteSpan& data);

// 写入ChunkData包的data字段（包含一个或多个原版的ChunkSection）
void write_chunk_data(ByteWriter& out, const world::Chunk& chunk, std::int32_t& primary_bit_mask);

// 读取ChunkData包的data字段（解析原版的ChunkSection并构建Chunk）
bool read_chunk_data(world::Chunk& chunk, const ByteSpan& data);

}
