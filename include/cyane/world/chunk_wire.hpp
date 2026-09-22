#pragma once

#include <cstdint>
#include <vector>

#include "cyane/core/bytes.hpp"
#include "cyane/world/chunk.hpp"

namespace cyane::world {

// 1.12.2 Chunk Data 的 data 字段：逐 section 写调色板 + 打包状态 + 光照，最后跟 256 字节生物群系
void write_chunk_data(ByteWriter& out, const Chunk& chunk, std::int32_t& primary_bit_mask);

}
