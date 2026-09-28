#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

#include "cyane/core/bytes.hpp"
#include "cyane/core/uuid.hpp"

namespace cyane::crypto {

// sha1 拼接后按 BigInteger(1, digest).toString(16) 格式化（协议 340 的 serverId，见 LoginListener$3）
[[nodiscard]] std::string server_id(ByteSpan shared_secret, ByteSpan public_der);

// Spigot/Paper 离线模式 UUID（RFC 4122 v3）：
//   1. 拼接 "OfflinePlayer:" + 用户名（区分大小写）
//   2. MD5 摘要
//   3. 强制 version 位（3）与 variant 位（8），得 name-based MD5 UUID
[[nodiscard]] cyane::Uuid offline_uuid(std::string_view username);

[[nodiscard]] Bytes random_bytes(std::size_t count);

}
