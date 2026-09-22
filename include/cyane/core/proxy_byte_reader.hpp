#pragma once

#include <cstdint>
#include <span>

namespace cyane::core {

class ProxyByteReader {
public:
    ProxyByteReader(std::span<const std::uint8_t> data) : data_(data), offset_(0) {}

    // 读取varint
    std::optional<std::int32_t> varint();
    // 读取u8
    std::optional<std::uint8_t> u8();
    // 读取i8
    std::optional<std::int8_t> i8();
    // 读取u16
    std::optional<std::uint16_t> u16();
    // 读取i16
    std::optional<std::int16_t> i16();
    // 读取u32
    std::optional<std::uint32_t> u32();
    // 读取i32
    std::optional<std::int32_t> i32();
    // 读取u64
    std::optional<std::uint64_t> u64();
    // 读取i64
    std::optional<std::int64_t> i64();
    // 读取f32
    std::optional<float> f32();
    // 读取f64
    std::optional<double> f64();
    // 读取布尔值
    std::optional<bool> boolean();
    // 读取字符串
    std::optional<std::string> string(std::size_t max_len);
    // 读取bytes
    std::optional<std::vector<std::uint8_t>> bytes(std::size_t len);

    // 获取剩余字节
    std::span<const std::uint8_t> rest() const {
        return data_.subspan(offset_);
    }

    // 获取偏移量
    std::size_t offset() const noexcept { return offset_; }

private:
    std::span<const std::uint8_t> data_;
    std::size_t offset_;
};

}
