#include "cyane/world/nbt.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <format>
#include <memory>
#include <type_traits>

#include "cyane/proto/frame.hpp"

namespace cyane::world::nbt {

namespace {

constexpr std::size_t kMaxNbtDepth = 64;
constexpr std::size_t kMaxNbtBytes = 8u << 20;  // 单区块 NBT 上限 8MB，防压缩炸弹

class Reader {
public:
    explicit Reader(ByteSpan data) : data_{data} {}

    [[nodiscard]] Result<Value> root() {
        auto tag = read_u8();
        if (!tag) {
            return std::unexpected{std::move(tag.error())};
        }
        if (static_cast<Tag>(*tag) != Tag::compound) {
            return make_error(ErrorCode::world, "nbt root is not a compound");
        }
        auto name = read_string();
        if (!name) {
            return std::unexpected{std::move(name.error())};
        }
        auto payload = read_payload(Tag::compound, 0);
        if (!payload) {
            return std::unexpected{std::move(payload.error())};
        }
        return std::move(*payload);
    }

    [[nodiscard]] std::size_t consumed() const noexcept { return pos_; }

private:
    [[nodiscard]] Result<std::uint8_t> read_u8() {
        if (pos_ + 1 > data_.size()) {
            return make_error(ErrorCode::world, "nbt truncated");
        }
        return std::to_integer<std::uint8_t>(data_[pos_++]);
    }

    template <typename T>
    [[nodiscard]] Result<T> read_be() {
        constexpr std::size_t kSize = sizeof(T);
        if (pos_ + kSize > data_.size()) {
            return make_error(ErrorCode::world, "nbt truncated");
        }
        const auto* raw = reinterpret_cast<const std::uint8_t*>(data_.data() + pos_);
        pos_ += kSize;
        if constexpr (std::is_floating_point_v<T>) {
            using Bits = std::conditional_t<sizeof(T) == 4, std::uint32_t, std::uint64_t>;
            return std::bit_cast<T>(load_be<Bits>(raw));
        } else if constexpr (sizeof(T) == 1) {
            return static_cast<T>(*raw);
        } else {
            return static_cast<T>(load_be<std::make_unsigned_t<T>>(raw));
        }
    }

    template <typename T>
    [[nodiscard]] static T load_be(const std::uint8_t* raw) noexcept {
        T out{};
        for (std::size_t i = 0; i < sizeof(T); ++i) {
            out = static_cast<T>((out << 8) | raw[i]);
        }
        return out;
    }

    [[nodiscard]] Result<std::string> read_string() {
        auto length = read_be<std::uint16_t>();
        if (!length) {
            return std::unexpected{std::move(length.error())};
        }
        if (pos_ + *length > data_.size()) {
            return make_error(ErrorCode::world, "nbt string truncated");
        }
        std::string text{reinterpret_cast<const char*>(data_.data() + pos_), *length};
        pos_ += *length;
        return text;
    }

    template <typename T>
    [[nodiscard]] Result<std::vector<T>> read_array(std::size_t width) {
        auto count = read_be<std::int32_t>();
        if (!count) {
            return std::unexpected{std::move(count.error())};
        }
        if (*count < 0) {
            return make_error(ErrorCode::world, "nbt negative array length");
        }
        const std::size_t total = static_cast<std::size_t>(*count) * width;
        if (pos_ + total > data_.size()) {
            return make_error(ErrorCode::world, "nbt array truncated");
        }
        std::vector<T> out;
        out.reserve(static_cast<std::size_t>(*count));
        for (std::int32_t i = 0; i < *count; ++i) {
            if constexpr (sizeof(T) == 1) {
                out.push_back(static_cast<T>(std::to_integer<std::uint8_t>(data_[pos_++])));
            } else {
                auto value = read_be<T>();
                if (!value) {
                    return std::unexpected{std::move(value.error())};
                }
                out.push_back(*value);
            }
        }
        return out;
    }

    [[nodiscard]] Result<Value> read_payload(Tag tag, std::size_t depth) {
        if (depth > kMaxNbtDepth) {
            return make_error(ErrorCode::world, "nbt nesting too deep");
        }
        Value out;
        out.type = tag;
        switch (tag) {
            case Tag::end:
                out.data = std::monostate{};
                break;
            case Tag::i8: {
                auto v = read_be<std::int8_t>();
                if (!v) return std::unexpected{std::move(v.error())};
                out.data = *v;
                break;
            }
            case Tag::i16: {
                auto v = read_be<std::int16_t>();
                if (!v) return std::unexpected{std::move(v.error())};
                out.data = *v;
                break;
            }
            case Tag::i32: {
                auto v = read_be<std::int32_t>();
                if (!v) return std::unexpected{std::move(v.error())};
                out.data = *v;
                break;
            }
            case Tag::i64: {
                auto v = read_be<std::int64_t>();
                if (!v) return std::unexpected{std::move(v.error())};
                out.data = *v;
                break;
            }
            case Tag::f32: {
                auto v = read_be<float>();
                if (!v) return std::unexpected{std::move(v.error())};
                out.data = *v;
                break;
            }
            case Tag::f64: {
                auto v = read_be<double>();
                if (!v) return std::unexpected{std::move(v.error())};
                out.data = *v;
                break;
            }
            case Tag::byte_array: {
                auto v = read_array<std::uint8_t>(1);
                if (!v) return std::unexpected{std::move(v.error())};
                Bytes raw(v->size());
                std::transform(v->begin(), v->end(), raw.begin(),
                               [](std::uint8_t b) { return static_cast<std::byte>(b); });
                out.data = std::move(raw);
                break;
            }
            case Tag::string: {
                auto v = read_string();
                if (!v) return std::unexpected{std::move(v.error())};
                out.data = std::move(*v);
                break;
            }
            case Tag::list: {
                auto element = read_u8();
                if (!element) return std::unexpected{std::move(element.error())};
                auto count = read_be<std::int32_t>();
                if (!count) return std::unexpected{std::move(count.error())};
                if (*count < 0) {
                    return make_error(ErrorCode::world, "nbt negative list length");
                }
                List items;
                items.reserve(static_cast<std::size_t>(*count));
                for (std::int32_t i = 0; i < *count; ++i) {
                    auto item = read_payload(static_cast<Tag>(*element), depth + 1);
                    if (!item) return std::unexpected{std::move(item.error())};
                    items.push_back(std::move(*item));
                }
                out.data = std::move(items);
                break;
            }
            case Tag::compound: {
                Compound fields;
                while (true) {
                    auto child_tag = read_u8();
                    if (!child_tag) return std::unexpected{std::move(child_tag.error())};
                    if (static_cast<Tag>(*child_tag) == Tag::end) {
                        break;
                    }
                    auto child_name = read_string();
                    if (!child_name) return std::unexpected{std::move(child_name.error())};
                    auto child = read_payload(static_cast<Tag>(*child_tag), depth + 1);
                    if (!child) return std::unexpected{std::move(child.error())};
                    fields.emplace_back(std::move(*child_name), std::move(*child));
                }
                out.data = std::move(fields);
                break;
            }
            case Tag::i32_array: {
                auto v = read_array<std::int32_t>(4);
                if (!v) return std::unexpected{std::move(v.error())};
                out.data = std::move(*v);
                break;
            }
            case Tag::i64_array: {
                auto v = read_array<std::int64_t>(8);
                if (!v) return std::unexpected{std::move(v.error())};
                out.data = std::move(*v);
                break;
            }
        }
        return out;
    }

    ByteSpan data_;
    std::size_t pos_{0};
};

class Writer {
public:
    void named(std::string_view name, const Value& value) {
        append_u8(static_cast<std::uint8_t>(value.type));
        append_name(name);
        payload(value);
    }

    [[nodiscard]] Bytes take() { return std::move(out_); }

private:
    void append_u8(std::uint8_t v) { out_.push_back(static_cast<std::byte>(v)); }

    template <typename T>
    void append_be(T value) {
        // 全部按位模式写出（bit_cast），再按大端逐字节；T 为 1/2/4/8 字节
        using Bits = std::conditional_t<sizeof(T) == 1, std::uint8_t,
                     std::conditional_t<sizeof(T) == 2, std::uint16_t,
                     std::conditional_t<sizeof(T) == 4, std::uint32_t, std::uint64_t>>>;
        auto bits = std::bit_cast<Bits>(value);
        for (int shift = static_cast<int>(sizeof(T)) * 8 - 8; shift >= 0; shift -= 8) {
            append_u8(static_cast<std::uint8_t>((bits >> shift) & 0xFF));
        }
    }

    void append_name(std::string_view name) {
        append_be<std::uint16_t>(static_cast<std::uint16_t>(name.size()));
        append(out_, name);
    }

    void payload(const Value& value) {
        switch (value.type) {
            case Tag::i8:
                append_be<std::int8_t>(std::get<std::int8_t>(value.data));
                break;
            case Tag::i16:
                append_be<std::int16_t>(std::get<std::int16_t>(value.data));
                break;
            case Tag::i32:
                append_be<std::int32_t>(std::get<std::int32_t>(value.data));
                break;
            case Tag::i64:
                append_be<std::int64_t>(std::get<std::int64_t>(value.data));
                break;
            case Tag::f32:
                append_be(std::get<float>(value.data));
                break;
            case Tag::f64:
                append_be(std::get<double>(value.data));
                break;
            case Tag::byte_array: {
                const auto& bytes = std::get<Bytes>(value.data);
                append_be<std::int32_t>(static_cast<std::int32_t>(bytes.size()));
                append(out_, ByteSpan{bytes});
                break;
            }
            case Tag::string: {
                const auto& text = std::get<std::string>(value.data);
                append_be<std::uint16_t>(static_cast<std::uint16_t>(text.size()));
                append(out_, text);
                break;
            }
            case Tag::list: {
                const auto& items = std::get<List>(value.data);
                const auto element = items.empty() ? Tag::end : items.front().type;
                append_u8(static_cast<std::uint8_t>(element));
                append_be<std::int32_t>(static_cast<std::int32_t>(items.size()));
                for (const auto& item : items) {
                    payload(item);
                }
                break;
            }
            case Tag::compound: {
                for (const auto& [name, field] : std::get<Compound>(value.data)) {
                    named(name, field);
                }
                append_u8(0);
                break;
            }
            case Tag::i32_array: {
                const auto& items = std::get<std::vector<std::int32_t>>(value.data);
                append_be<std::int32_t>(static_cast<std::int32_t>(items.size()));
                for (const auto item : items) {
                    append_be<std::int32_t>(item);
                }
                break;
            }
            case Tag::i64_array: {
                const auto& items = std::get<std::vector<std::int64_t>>(value.data);
                append_be<std::int32_t>(static_cast<std::int32_t>(items.size()));
                for (const auto item : items) {
                    append_be<std::int64_t>(item);
                }
                break;
            }
            case Tag::end:
                break;
        }
    }

    Bytes out_;
};

} // namespace

std::optional<std::int64_t> Value::scalar() const noexcept {
    switch (type) {
        case Tag::i8: return static_cast<std::int64_t>(std::get<std::int8_t>(data));
        case Tag::i16: return static_cast<std::int64_t>(std::get<std::int16_t>(data));
        case Tag::i32: return static_cast<std::int64_t>(std::get<std::int32_t>(data));
        case Tag::i64: return std::get<std::int64_t>(data);
        default: return std::nullopt;
    }
}

std::optional<std::string_view> Value::text() const noexcept {
    if (const auto* s = std::get_if<std::string>(&data)) {
        return std::string_view{*s};
    }
    return std::nullopt;
}

Result<Value> parse(ByteSpan data) {
    if (data.size() > kMaxNbtBytes) {
        return make_error(ErrorCode::world, "nbt payload too large");
    }
    Reader reader{data};
    return reader.root();
}

Result<Value> parse_region_payload(ByteSpan payload) {
    if (payload.empty()) {
        return make_error(ErrorCode::world, "empty region payload");
    }
    const auto version = std::to_integer<std::uint8_t>(payload.front());
    const auto body = payload.subspan(1);
    if (version == 1) {
        auto inflated = proto::inflate_dynamic(body, kMaxNbtBytes, true);
        if (!inflated) {
            return std::unexpected{std::move(inflated.error())};
        }
        return parse(ByteSpan{*inflated});
    }
    if (version == 2) {
        auto inflated = proto::inflate_dynamic(body, kMaxNbtBytes);
        if (!inflated) {
            return std::unexpected{std::move(inflated.error())};
        }
        return parse(ByteSpan{*inflated});
    }
    return make_error(ErrorCode::world, std::format("unsupported region chunk version {}", version));
}

Result<Value> parse_compressed(ByteSpan data) {
    if (data.size() >= 2 && data[0] == std::byte{0x1F} && data[1] == std::byte{0x8B}) {
        auto inflated = proto::inflate_dynamic(data, kMaxNbtBytes, true);
        if (!inflated) {
            return std::unexpected{std::move(inflated.error())};
        }
        return parse(ByteSpan{*inflated});
    }
    if (!data.empty() && data[0] == std::byte{0x0A}) {
        return parse(data);  // 未压缩 raw NBT
    }
    if (!data.empty() && data[0] == std::byte{0x78}) {
        auto inflated = proto::inflate_dynamic(data, kMaxNbtBytes);
        if (!inflated) {
            return std::unexpected{std::move(inflated.error())};
        }
        return parse(ByteSpan{*inflated});
    }
    return make_error(ErrorCode::world, "unknown NBT container format");
}

Result<Bytes> serialize(std::string_view root_name, const Value& root) {
    if (root.type != Tag::compound) {
        return make_error(ErrorCode::world, "nbt root must be a compound");
    }
    Writer writer;
    writer.named(root_name, root);
    return writer.take();
}

} // namespace cyane::world::nbt