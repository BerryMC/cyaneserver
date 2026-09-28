#include "cyane/core/uuid.hpp"

namespace cyane {

namespace {
constexpr char kHex[] = "0123456789abcdef";

[[nodiscard]] int hex_digit(char c) noexcept {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}
} // namespace

Uuid Uuid::parse(std::string_view text) {
    // 接受 8-4-4-4-12（带 dash）或 32 位连续十六进制
    std::uint8_t bytes[16]{};
    std::size_t nibble = 0;
    for (const char c : text) {
        if (c == '-') {
            continue;
        }
        const int d = hex_digit(c);
        if (d < 0 || nibble >= 32) {
            return {};
        }
        if ((nibble & 1) == 0) {
            bytes[nibble >> 1] = static_cast<std::uint8_t>(d << 4);
        } else {
            bytes[nibble >> 1] = static_cast<std::uint8_t>(bytes[nibble >> 1] | d);
        }
        ++nibble;
    }
    if (nibble != 32) {
        return {};
    }
    Bytes out{};
    for (std::size_t i = 0; i < out.size(); ++i) {
        out[i] = bytes[i];
    }
    return Uuid{out};
}

std::string Uuid::hex() const {
    std::string out;
    out.reserve(32);
    for (const auto byte : bytes_) {
        out.push_back(kHex[byte >> 4]);
        out.push_back(kHex[byte & 0x0F]);
    }
    return out;
}

std::string Uuid::dashed() const {
    const auto h = hex();
    std::string out;
    out.reserve(36);
    for (std::size_t i = 0; i < h.size(); ++i) {
        if (i == 8 || i == 12 || i == 16 || i == 20) {
            out.push_back('-');
        }
        out.push_back(h[i]);
    }
    return out;
}

} // namespace cyane
