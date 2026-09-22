#include "cyane/core/proxy_byte_reader.hpp"
#include "cyane/core/bytes.hpp"
#include <stdexcept>

namespace cyane::core {

std::optional<std::int32_t> ProxyByteReader::varint() {
    std::int32_t value = 0;
    std::size_t shift = 0;
    while (offset_ < data_.size()) {
        std::uint8_t byte = data_[offset_++];
        value |= (byte & 0x7F) << shift;
        if ((byte & 0x80) == 0) {
            return value;
        }
        shift += 7;
        if (shift >= 35 * 7) {
            return std::nullopt;
        }
    }
    return std::nullopt;
}

std::optional<std::uint8_t> ProxyByteReader::u8() {
    if (offset_ >= data_.size()) {
        return std::nullopt;
    }
    return data_[offset_++];
}

std::optional<std::int8_t> ProxyByteReader::i8() {
    if (offset_ >= data_.size()) {
        return std::nullopt;
    }
    return static_cast<std::int8_t>(data_[offset_++]);
}

std::optional<std::uint16_t> ProxyByteReader::u16() {
    if (offset_ + 2 > data_.size()) {
        return std::nullopt;
    }
    std::uint16_t value = (static_cast<std::uint16_t>(data_[offset_]) << 8) |
                          (static_cast<std::uint16_t>(data_[offset_ + 1]));
    offset_ += 2;
    return value;
}

std::optional<std::int16_t> ProxyByteReader::i16() {
    if (offset_ + 2 > data_.size()) {
        return std::nullopt;
    }
    std::int16_t value = (static_cast<std::int16_t>(data_[offset_]) << 8) |
                         (static_cast<std::int16_t>(data_[offset_ + 1]));
    offset_ += 2;
    return value;
}

std::optional<std::uint32_t> ProxyByteReader::u32() {
    if (offset_ + 4 > data_.size()) {
        return std::nullopt;
    }
    std::uint32_t value = (static_cast<std::uint32_t>(data_[offset_]) << 24) |
                          (static_cast<std::uint32_t>(data_[offset_ + 1]) << 16) |
                          (static_cast<std::uint32_t>(data_[offset_ + 2]) << 8) |
                          (static_cast<std::uint32_t>(data_[offset_ + 3]));
    offset_ += 4;
    return value;
}

std::optional<std::int32_t> ProxyByteReader::i32() {
    if (offset_ + 4 > data_.size()) {
        return std::nullopt;
    }
    std::int32_t value = (static_cast<std::int32_t>(data_[offset_]) << 24) |
                         (static_cast<std::int32_t>(data_[offset_ + 1]) << 16) |
                         (static_cast<std::int32_t>(data_[offset_ + 2]) << 8) |
                         (static_cast<std::int32_t>(data_[offset_ + 3]));
    offset_ += 4;
    return value;
}

std::optional<std::uint64_t> ProxyByteReader::u64() {
    if (offset_ + 8 > data_.size()) {
        return std::nullopt;
    }
    std::uint64_t value = (static_cast<std::uint64_t>(data_[offset_]) << 56) |
                          (static_cast<std::uint64_t>(data_[offset_ + 1]) << 48) |
                          (static_cast<std::uint64_t>(data_[offset_ + 2]) << 40) |
                          (static_cast<std::uint64_t>(data_[offset_ + 3]) << 32) |
                          (static_cast<std::uint64_t>(data_[offset_ + 4]) << 24) |
                          (static_cast<std::uint64_t>(data_[offset_ + 5]) << 16) |
                          (static_cast<std::uint64_t>(data_[offset_ + 6]) << 8) |
                          (static_cast<std::uint64_t>(data_[offset_ + 7]));
    offset_ += 8;
    return value;
}

std::optional<std::int64_t> ProxyByteReader::i64() {
    if (offset_ + 8 > data_.size()) {
        return std::nullopt;
    }
    std::int64_t value = (static_cast<std::int64_t>(data_[offset_]) << 56) |
                         (static_cast<std::int64_t>(data_[offset_ + 1]) << 48) |
                         (static_cast<std::int64_t>(data_[offset_ + 2]) << 40) |
                         (static_cast<std::int64_t>(data_[offset_ + 3]) << 32) |
                         (static_cast<std::int64_t>(data_[offset_ + 4]) << 24) |
                         (static_cast<std::int64_t>(data_[offset_ + 5]) << 16) |
                         (static_cast<std::int64_t>(data_[offset_ + 6]) << 8) |
                         (static_cast<std::int64_t>(data_[offset_ + 7]));
    offset_ += 8;
    return value;
}

std::optional<float> ProxyByteReader::f32() {
    auto val = u32();
    if (!val) {
        return std::nullopt;
    }
    float result;
    std::memcpy(&result, &(*val), sizeof(float));
    return result;
}

std::optional<double> ProxyByteReader::f64() {
    auto val = u64();
    if (!val) {
        return std::nullopt;
    }
    double result;
    std::memcpy(&result, &(*val), sizeof(double));
    return result;
}

std::optional<bool> ProxyByteReader::boolean() {
    auto val = u8();n    if (!val) {
        return std::nullopt;
    }
    return *val != 0;
}

std::optional<std::string> ProxyByteReader::string(std::size_t max_len) {
    auto length = varint();
    if (!length || *length > max_len || offset_ + *length > data_.size()) {
        return std::nullopt;
    }
    std::string result(reinterpret_cast<const char*>(&data_[offset_]), *length);
    offset_ += *length;
    return result;
}

std::optional<std::vector<std::uint8_t>> ProxyByteReader::bytes(std::size_t len) {
    if (offset_ + len > data_.size()) {
        return std::nullopt;
    }
    std::vector<std::uint8_t> result(data_.begin() + offset_, data_.begin() + offset_ + len);
    offset_ += len;
    return result;
}

}
