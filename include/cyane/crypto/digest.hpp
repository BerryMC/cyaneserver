#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

#include "cyane/core/bytes.hpp"

namespace cyane::crypto {

// sha1 拼接后按 BigInteger(1, digest).toString(16) 格式化（协议 340 的 serverId，见 LoginListener$3）
[[nodiscard]] std::string server_id(ByteSpan shared_secret, ByteSpan public_der);

// UUID.nameUUIDFromBytes("OfflinePlayer:" + name)，RFC 4122 v3
[[nodiscard]] std::string offline_uuid(std::string_view username);

// 8-4-4-4-12 dashed UUID → 16 bytes
[[nodiscard]] std::array<std::uint8_t, 16> parse_uuid_string(std::string_view dashed);

// 16 bytes → 8-4-4-4-12 dashed UUID
[[nodiscard]] std::string to_uuid_string(const std::array<std::uint8_t, 16>& bytes);

// 无连字符 32 位 hex → 8-4-4-4-12
[[nodiscard]] std::string uuid_with_dashes(std::string_view compact);

[[nodiscard]] Bytes random_bytes(std::size_t count);

}
