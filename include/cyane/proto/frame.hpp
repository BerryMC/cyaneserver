#pragma once

#include <array>
#include <memory>
#include <optional>

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
// gzip 容器（RFC 1952）：原版 level.dat / playerdata/*.dat 用
[[nodiscard]] Result<Bytes> deflate_gzip(ByteSpan input, int level);
[[nodiscard]] Result<Bytes> inflate(ByteSpan input, std::size_t output_size);
// 输出大小未知的流式解压（Anvil 区块）：gzip=true 处理版本字节 1 的 gzip 载荷，
// 否则按 zlib 载荷（版本字节 2）。max_output 上限防压缩炸弹。
[[nodiscard]] Result<Bytes> inflate_dynamic(ByteSpan input, std::size_t max_output, bool gzip = false);

}
