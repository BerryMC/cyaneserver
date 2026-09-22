#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "cyane/core/error.hpp"

namespace cyane {

using ByteSpan = std::span<const std::byte>;
using MutableByteSpan = std::span<std::byte>;
using Bytes = std::vector<std::byte>;

inline constexpr std::size_t kMaxStringChars = 32767;
inline constexpr std::size_t kMaxStringBytes = kMaxStringChars * 4;

[[nodiscard]] inline ByteSpan as_bytes(std::string_view text) noexcept {
    return ByteSpan{reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

[[nodiscard]] inline std::string_view as_text(ByteSpan bytes) noexcept {
    return std::string_view{reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

[[nodiscard]] inline std::string as_hex(ByteSpan bytes) noexcept {
    constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 3);
    for (const std::byte b : bytes) {
        const auto v = std::to_integer<std::uint8_t>(b);
        out.push_back(kHex[v >> 4]);
        out.push_back(kHex[v & 0x0F]);
        out.push_back(' ');
    }
    return out;
}

inline void append(Bytes& out, ByteSpan bytes) { out.insert(out.end(), bytes.begin(), bytes.end()); }
inline void append(Bytes& out, std::string_view text) { append(out, as_bytes(text)); }

inline void append_varint(Bytes& out, std::int32_t value) {
    auto bits = static_cast<std::uint32_t>(value);
    for (;;) {
        if ((bits & ~0x7Fu) == 0) {
            out.push_back(static_cast<std::byte>(bits));
            return;
        }
        out.push_back(static_cast<std::byte>((bits & 0x7F) | 0x80));
        bits >>= 7;
    }
}

[[nodiscard]] constexpr std::size_t varint_size(std::int32_t value) noexcept {
    auto bits = static_cast<std::uint32_t>(value);
    std::size_t size = 1;
    while ((bits & ~0x7Fu) != 0) {
        bits >>= 7;
        ++size;
    }
    return size;
}

inline void append_varlong(Bytes& out, std::int64_t value) {
    auto bits = static_cast<std::uint64_t>(value);
    for (;;) {
        if ((bits & ~0x7Full) == 0) {
            out.push_back(static_cast<std::byte>(bits));
            return;
        }
        out.push_back(static_cast<std::byte>((bits & 0x7F) | 0x80));
        bits >>= 7;
    }
}

// 1.12.2 的位置编码（与 BlockPosition.asLong 一致）：x(26) << 38 | y(12) << 26 | z(26)
[[nodiscard]] inline std::int64_t encode_position(std::int32_t x, std::int32_t y, std::int32_t z) noexcept {
    return (static_cast<std::int64_t>(x & 0x3FFFFFF) << 38) | (static_cast<std::int64_t>(y & 0xFFF) << 26) |
           static_cast<std::int64_t>(z & 0x3FFFFFF);
}

[[nodiscard]] inline std::int32_t position_x(std::int64_t packed) noexcept {
    return static_cast<std::int32_t>(packed >> 38);
}

[[nodiscard]] inline std::int32_t position_y(std::int64_t packed) noexcept {
    return static_cast<std::int32_t>(packed << 26 >> 52);
}

[[nodiscard]] inline std::int32_t position_z(std::int64_t packed) noexcept {
    return static_cast<std::int32_t>(packed << 38 >> 38);
}

[[nodiscard]] inline bool is_valid_utf8(std::string_view text) noexcept {
    std::size_t index = 0;
    while (index < text.size()) {
        const auto lead = static_cast<std::uint8_t>(text[index]);
        if (lead < 0x80) {
            ++index;
            continue;
        }
        std::size_t extra = 0;
        std::uint32_t code_point = 0;
        if ((lead & 0xE0) == 0xC0) {
            extra = 1;
            code_point = lead & 0x1Fu;
        } else if ((lead & 0xF0) == 0xE0) {
            extra = 2;
            code_point = lead & 0x0Fu;
        } else if ((lead & 0xF8) == 0xF0) {
            extra = 3;
            code_point = lead & 0x07u;
        } else {
            return false;
        }
        if (index + extra >= text.size()) {
            return false;
        }
        for (std::size_t i = 1; i <= extra; ++i) {
            const auto byte = static_cast<std::uint8_t>(text[index + i]);
            if ((byte & 0xC0) != 0x80) {
                return false;
            }
            code_point = (code_point << 6) | (byte & 0x3Fu);
        }
        constexpr std::uint32_t kMinOne = 0x80;
        constexpr std::uint32_t kMinTwo = 0x800;
        constexpr std::uint32_t kMinThree = 0x10000;
        constexpr std::uint32_t kMaxCodePoint = 0x10FFFF;
        if ((extra == 1 && code_point < kMinOne) || (extra == 2 && code_point < kMinTwo) ||
            (extra == 3 && code_point < kMinThree) || code_point > kMaxCodePoint ||
            (code_point >= 0xD800 && code_point <= 0xDFFF)) {
            return false;
        }
        index += extra + 1;
    }
    return true;
}

class ByteReader {
public:
    explicit ByteReader(ByteSpan data) noexcept : data_{data} {}

    [[nodiscard]] std::size_t remaining() const noexcept { return data_.size() - offset_; }
    [[nodiscard]] std::size_t offset() const noexcept { return offset_; }
    [[nodiscard]] bool empty() const noexcept { return offset_ >= data_.size(); }
    [[nodiscard]] ByteSpan rest() const noexcept { return data_.subspan(offset_); }

    void skip(std::size_t count) noexcept { offset_ += count; }

    [[nodiscard]] Result<std::uint8_t> u8() noexcept {
        if (remaining() < 1) {
            return underflow(1);
        }
        return std::to_integer<std::uint8_t>(data_[offset_++]);
    }

    [[nodiscard]] Result<std::int8_t> i8() noexcept {
        auto value = u8();
        if (!value) {
            return std::unexpected{std::move(value.error())};
        }
        return static_cast<std::int8_t>(*value);
    }

    [[nodiscard]] Result<bool> boolean() noexcept {
        auto value = u8();
        if (!value) {
            return std::unexpected{std::move(value.error())};
        }
        return *value != 0;
    }

    template <typename T>
    [[nodiscard]] Result<T> big_endian() noexcept {
        static_assert(sizeof(T) >= 2 && std::is_trivially_copyable_v<T>);
        if (remaining() < sizeof(T)) {
            return underflow(sizeof(T));
        }
        std::array<std::byte, sizeof(T)> raw{};
        std::memcpy(raw.data(), data_.data() + offset_, sizeof(T));
        offset_ += sizeof(T);
        if constexpr (std::endian::native == std::endian::little) {
            std::ranges::reverse(raw);
        }
        return std::bit_cast<T>(raw);
    }

    [[nodiscard]] Result<std::uint16_t> u16() noexcept { return big_endian<std::uint16_t>(); }
    [[nodiscard]] Result<std::int16_t> i16() noexcept { return big_endian<std::int16_t>(); }
    [[nodiscard]] Result<std::int32_t> i32() noexcept { return big_endian<std::int32_t>(); }
    [[nodiscard]] Result<std::int64_t> i64() noexcept { return big_endian<std::int64_t>(); }

    [[nodiscard]] Result<float> f32() noexcept {
        auto bits = big_endian<std::uint32_t>();
        if (!bits) {
            return std::unexpected{std::move(bits.error())};
        }
        return std::bit_cast<float>(*bits);
    }

    [[nodiscard]] Result<double> f64() noexcept {
        auto bits = big_endian<std::uint64_t>();
        if (!bits) {
            return std::unexpected{std::move(bits.error())};
        }
        return std::bit_cast<double>(*bits);
    }

    [[nodiscard]] Result<std::int32_t> varint() noexcept {
        std::uint32_t value = 0;
        for (int shift = 0; shift < 35; shift += 7) {
            if (offset_ >= data_.size()) {
                return underflow(1);
            }
            const auto byte = std::to_integer<std::uint8_t>(data_[offset_++]);
            value |= static_cast<std::uint32_t>(byte & 0x7F) << shift;
            if ((byte & 0x80) == 0) {
                return static_cast<std::int32_t>(value);
            }
        }
        return make_error(ErrorCode::protocol, "VarInt longer than 5 bytes");
    }

    [[nodiscard]] Result<std::int64_t> varlong() noexcept {
        std::uint64_t value = 0;
        for (int shift = 0; shift < 70; shift += 7) {
            if (offset_ >= data_.size()) {
                return underflow(1);
            }
            const auto byte = std::to_integer<std::uint8_t>(data_[offset_++]);
            value |= static_cast<std::uint64_t>(byte & 0x7F) << shift;
            if ((byte & 0x80) == 0) {
                return static_cast<std::int64_t>(value);
            }
        }
        return make_error(ErrorCode::protocol, "VarLong longer than 10 bytes");
    }

    [[nodiscard]] Result<std::string> string(std::size_t max_bytes = kMaxStringBytes) noexcept {
        auto length = varint();
        if (!length) {
            return std::unexpected{std::move(length.error())};
        }
        if (*length < 0) {
            return make_error(ErrorCode::protocol, "negative string length");
        }
        const auto count = static_cast<std::size_t>(*length);
        if (count > max_bytes) {
            return make_error(
                ErrorCode::protocol, "string of " + std::to_string(count) + " bytes exceeds limit");
        }
        if (remaining() < count) {
            return underflow(count);
        }
        std::string text{as_text(data_.subspan(offset_, count))};
        offset_ += count;
        if (!is_valid_utf8(text)) {
            return make_error(ErrorCode::protocol, "string is not valid UTF-8");
        }
        return text;
    }

    [[nodiscard]] Result<ByteSpan> bytes(std::size_t count) noexcept {
        if (remaining() < count) {
            return underflow(count);
        }
        const auto slice = data_.subspan(offset_, count);
        offset_ += count;
        return slice;
    }

private:
    [[nodiscard]] std::unexpected<Error> underflow(std::size_t wanted) const {
        return make_error(ErrorCode::protocol,
                          "need " + std::to_string(wanted) + " bytes, " + std::to_string(remaining()) + " available");
    }

    ByteSpan data_;
    std::size_t offset_{0};
};

class ByteWriter {
public:
    ByteWriter() = default;
    explicit ByteWriter(std::size_t reserve) { buffer_.reserve(reserve); }

    [[nodiscard]] const Bytes& data() const noexcept { return buffer_; }
    [[nodiscard]] Bytes take() noexcept { return std::move(buffer_); }
    [[nodiscard]] std::size_t size() const noexcept { return buffer_.size(); }

    void clear() noexcept { buffer_.clear(); }

    void u8(std::uint8_t value) { buffer_.push_back(static_cast<std::byte>(value)); }
    void boolean(bool value) { u8(value ? 1 : 0); }

    template <typename T>
    void big_endian(T value) {
        static_assert(sizeof(T) >= 2 && std::is_trivially_copyable_v<T>);
        auto raw = std::bit_cast<std::array<std::byte, sizeof(T)>>(value);
        if constexpr (std::endian::native == std::endian::little) {
            std::ranges::reverse(raw);
        }
        buffer_.insert(buffer_.end(), raw.begin(), raw.end());
    }

    void u16(std::uint16_t value) { big_endian(value); }
    void i16(std::int16_t value) { big_endian(value); }
    void i32(std::int32_t value) { big_endian(value); }
    void i64(std::int64_t value) { big_endian(value); }
    void f32(float value) { big_endian(std::bit_cast<std::uint32_t>(value)); }
    void f64(double value) { big_endian(std::bit_cast<std::uint64_t>(value)); }

    void varint(std::int32_t value) { append_varint(buffer_, value); }
    void varlong(std::int64_t value) { append_varlong(buffer_, value); }

    void string(std::string_view text) {
        const auto count = std::min(text.size(), kMaxStringBytes);
        varint(static_cast<std::int32_t>(count));
        append(buffer_, text.substr(0, count));
    }

    void bytes(ByteSpan value) { append(buffer_, value); }

    void position(std::int32_t x, std::int32_t y, std::int32_t z) { i64(encode_position(x, y, z)); }

private:
    Bytes buffer_;
};

}
