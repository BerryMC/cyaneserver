#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace cyane {

// RFC 4122 128 位 UUID。
// Spigot/Paper 离线模式身份：md5("OfflinePlayer:"+用户名) 并强制 version 3 + variant 位，
// 见 crypto::offline_uuid()。
class Uuid final {
public:
    using Bytes = std::array<std::uint8_t, 16>;

    Uuid() noexcept = default;
    [[nodiscard]] constexpr explicit Uuid(Bytes bytes) noexcept : bytes_{bytes} {}

    // 解析 36 字符带 dash 或 32 位十六进制（大小写均可）；非法输入返回空 UUID
    [[nodiscard]] static Uuid parse(std::string_view text);
    // 原始 16 字节（网络/存储序）
    [[nodiscard]] static Uuid from_bytes(Bytes bytes) noexcept {
        return Uuid{bytes};
    }
    // 由 MD5 摘要生成 version 3（基于名称）UUID：置版本位与变体位
    [[nodiscard]] static Uuid md5_v3(Bytes md5) noexcept {
        auto b = md5;
        b[6] = static_cast<std::uint8_t>((b[6] & 0x0F) | 0x30);
        b[8] = static_cast<std::uint8_t>((b[8] & 0x3F) | 0x80);
        return Uuid{b};
    }

    [[nodiscard]] bool is_null() const noexcept {
        for (const auto byte : bytes_) {
            if (byte != 0) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] Bytes bytes() const noexcept { return bytes_; }
    [[nodiscard]] std::string hex() const;      // 32 位小写十六进制
    [[nodiscard]] std::string dashed() const;    // 8-4-4-4-12 带 dash

private:
    Bytes bytes_{};
};

[[nodiscard]] constexpr bool operator==(Uuid a, Uuid b) noexcept { return a.bytes() == b.bytes(); }
[[nodiscard]] constexpr bool operator!=(Uuid a, Uuid b) noexcept { return a.bytes() != b.bytes(); }

} // namespace cyane

template <>
struct std::hash<cyane::Uuid> {
    [[nodiscard]] std::size_t operator()(cyane::Uuid const& uuid) const noexcept {
        const auto b = uuid.bytes();
        std::uint64_t hi = 0, lo = 0;
        for (std::size_t i = 0; i < 8; ++i) {
            hi = (hi << 8) | b[i];
        }
        for (std::size_t i = 8; i < 16; ++i) {
            lo = (lo << 8) | b[i];
        }
        return std::hash<std::uint64_t>{}(hi) * 1000003u + std::hash<std::uint64_t>{}(lo);
    }
};
