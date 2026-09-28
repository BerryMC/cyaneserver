#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "cyane/core/bytes.hpp"
#include "cyane/core/error.hpp"

namespace cyane::world::nbt {

// 1.12.2 Anvil 的 NBT（大端、命名标签）。网络 NBT 是另一套编码，两者不得混用。
enum class Tag : std::uint8_t {
    end = 0,
    i8 = 1,
    i16 = 2,
    i32 = 3,
    i64 = 4,
    f32 = 5,
    f64 = 6,
    byte_array = 7,
    string = 8,
    list = 9,
    compound = 10,
    i32_array = 11,
    i64_array = 12,
};

class Value;
using List = std::vector<Value>;
// 保序 compound：Anvil 键序不影响语义，但保序让输出稳定可测
using Compound = std::vector<std::pair<std::string, Value>>;

class Value {
public:
    using Data = std::variant<std::monostate,
                              std::int8_t,
                              std::int16_t,
                              std::int32_t,
                              std::int64_t,
                              float,
                              double,
                              Bytes,
                              std::string,
                              List,
                              Compound,
                              std::vector<std::int32_t>,
                              std::vector<std::int64_t>>;

    Tag type{Tag::end};
    Data data{};

    [[nodiscard]] bool is(Tag tag) const noexcept { return type == tag; }

    template <typename T>
    [[nodiscard]] const T* get_if() const noexcept {
        return std::get_if<T>(&data);
    }

    [[nodiscard]] const Value* find(std::string_view key) const noexcept {
        const auto* fields = std::get_if<Compound>(&data);
        if (fields == nullptr) {
            return nullptr;
        }
        for (const auto& [name, value] : *fields) {
            if (name == key) {
                return &value;
            }
        }
        return nullptr;
    }

    // 取标量（整数族之间窄化兼容：i8/i16/i32/i64 互通）
    [[nodiscard]] std::optional<std::int64_t> scalar() const noexcept;
    [[nodiscard]] std::optional<std::string_view> text() const noexcept;
};

[[nodiscard]] inline Value make_i8(std::int8_t v) noexcept { return {Tag::i8, v}; }
[[nodiscard]] inline Value make_i16(std::int16_t v) noexcept { return {Tag::i16, v}; }
[[nodiscard]] inline Value make_i32(std::int32_t v) noexcept { return {Tag::i32, v}; }
[[nodiscard]] inline Value make_i64(std::int64_t v) noexcept { return {Tag::i64, v}; }
[[nodiscard]] inline Value make_string(std::string v) { return {Tag::string, std::move(v)}; }
[[nodiscard]] inline Value make_byte_array(Bytes v) { return {Tag::byte_array, std::move(v)}; }
[[nodiscard]] inline Value make_list(List v) { return {Tag::list, std::move(v)}; }
[[nodiscard]] inline Value make_compound(Compound v) { return {Tag::compound, std::move(v)}; }

// 解析根为命名 compound 的 NBT（Anvil 区块载荷）
[[nodiscard]] Result<Value> parse(ByteSpan data);
// 解析压缩载荷（版本字节 1=gzip / 2=zlib），返回解压后的 NBT 根
[[nodiscard]] Result<Value> parse_region_payload(ByteSpan payload);

// 编码为「命名 compound 根」的字节流（不含 region 版本字节/压缩，由调用方处理）
[[nodiscard]] Result<Bytes> serialize(std::string_view root_name, const Value& root);

} // namespace cyane::world::nbt