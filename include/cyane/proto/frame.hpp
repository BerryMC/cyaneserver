#pragma once

#include "cyane/core/bytes.hpp"
#include "cyane/core/error.hpp"

namespace cyane::proto {

// 协议 340 的长度前缀为 VarInt，其值域上限即单帧上限
inline constexpr std::int32_t kMaxFrameBytes = 2'097'151;
inline constexpr std::int32_t kDefaultCompressionThreshold = 256;
inline constexpr int kDefaultCompressionLevel = 6;

struct DecodedFrame {
    std::int32_t packet_id{0};
    ByteSpan payload{};
};

// threshold < 0 表示压缩未启用
[[nodiscard]] Result<DecodedFrame> decode_frame(ByteSpan body, Bytes& scratch, std::int32_t threshold);

// 把 packet_id 与 fields 编码为完整帧（含长度前缀）追加到 out
void encode_frame(Bytes& out, std::int32_t packet_id, ByteSpan fields, std::int32_t threshold);

[[nodiscard]] Result<Bytes> deflate(ByteSpan input, int level);
[[nodiscard]] Result<Bytes> inflate(ByteSpan input, std::size_t output_size);

}
