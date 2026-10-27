#pragma once

#include <charconv>
#include <cstdint>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace cyane::core {

// 极简 JSON 解析器（仅服务端数据文件用：loot_tables / advancements 等）
// 只做结构解析，不做 schema 校验；解析失败返回 nullopt 而不是抛异常。
class JsonValue {
public:
    enum class Type : std::uint8_t { Null, Bool, Number, String, Array, Object };

    using Array = std::vector<JsonValue>;
    using Object = std::unordered_map<std::string, JsonValue>;

    JsonValue() = default;

    [[nodiscard]] Type type() const noexcept { return type_; }
    [[nodiscard]] bool is_null() const noexcept { return type_ == Type::Null; }
    [[nodiscard]] bool is_object() const noexcept { return type_ == Type::Object; }
    [[nodiscard]] bool is_array() const noexcept { return type_ == Type::Array; }
    [[nodiscard]] bool is_string() const noexcept { return type_ == Type::String; }
    [[nodiscard]] bool is_number() const noexcept { return type_ == Type::Number; }
    [[nodiscard]] bool is_bool() const noexcept { return type_ == Type::Bool; }

    [[nodiscard]] bool as_bool(bool fallback = false) const noexcept {
        return type_ == Type::Bool ? bool_ : fallback;
    }

    [[nodiscard]] double as_number(double fallback = 0.0) const noexcept {
        return type_ == Type::Number ? number_ : fallback;
    }

    [[nodiscard]] std::int64_t as_int(std::int64_t fallback = 0) const noexcept {
        return type_ == Type::Number ? static_cast<std::int64_t>(number_) : fallback;
    }

    [[nodiscard]] std::string_view as_string(std::string_view fallback = {}) const noexcept {
        return type_ == Type::String ? std::string_view{string_} : fallback;
    }

    [[nodiscard]] const Array& as_array() const noexcept { return array_; }
    [[nodiscard]] const Object& as_object() const noexcept { return object_; }

    // 对象成员查找（非对象或缺失返回 nullptr）
    [[nodiscard]] const JsonValue* find(std::string_view key) const noexcept {
        if (type_ != Type::Object) return nullptr;
        const auto it = object_.find(std::string{key});
        return it == object_.end() ? nullptr : &it->second;
    }

    [[nodiscard]] const JsonValue* at(std::size_t index) const noexcept {
        return (type_ == Type::Array && index < array_.size()) ? &array_[index] : nullptr;
    }

    // ---- 便捷取值（缺失/类型不符时返回 fallback，对应 vanilla JsonUtils.*）----
    [[nodiscard]] bool get_bool(std::string_view key, bool fallback = false) const noexcept {
        const auto* v = find(key);
        return v ? v->as_bool(fallback) : fallback;
    }

    [[nodiscard]] std::int64_t get_int(std::string_view key, std::int64_t fallback = 0) const noexcept {
        const auto* v = find(key);
        return v ? v->as_int(fallback) : fallback;
    }

    [[nodiscard]] double get_number(std::string_view key, double fallback = 0.0) const noexcept {
        const auto* v = find(key);
        return v ? v->as_number(fallback) : fallback;
    }

    [[nodiscard]] std::string_view get_string(std::string_view key, std::string_view fallback = {}) const noexcept {
        const auto* v = find(key);
        return v ? v->as_string(fallback) : fallback;
    }

    [[nodiscard]] const Array* get_array(std::string_view key) const noexcept {
        const auto* v = find(key);
        return (v && v->is_array()) ? &v->as_array() : nullptr;
    }

    [[nodiscard]] const JsonValue* get_object(std::string_view key) const noexcept {
        const auto* v = find(key);
        return (v && v->is_object()) ? v : nullptr;
    }

    // 便捷构造（解析器内部与测试用）
    static JsonValue make_null() { return JsonValue{}; }
    static JsonValue make_bool(bool v) { JsonValue j; j.type_ = Type::Bool; j.bool_ = v; return j; }
    static JsonValue make_number(double v) { JsonValue j; j.type_ = Type::Number; j.number_ = v; return j; }
    static JsonValue make_string(std::string v) { JsonValue j; j.type_ = Type::String; j.string_ = std::move(v); return j; }
    static JsonValue make_array(Array v) { JsonValue j; j.type_ = Type::Array; j.array_ = std::move(v); return j; }
    static JsonValue make_object(Object v) { JsonValue j; j.type_ = Type::Object; j.object_ = std::move(v); return j; }

    // 解析 UTF-8 JSON 文本；语法错误返回 nullopt
    [[nodiscard]] static std::optional<JsonValue> parse(std::string_view text) {
        std::size_t pos = 0;
        JsonValue root;
        if (!parse_value(text, pos, root)) return std::nullopt;
        skip_ws(text, pos);
        if (pos != text.size()) return std::nullopt;
        return root;
    }

private:
    static void skip_ws(std::string_view text, std::size_t& pos) noexcept {
        while (pos < text.size()) {
            const char c = text[pos];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ++pos;
            } else {
                break;
            }
        }
    }

    static bool parse_value(std::string_view text, std::size_t& pos, JsonValue& out);

    static bool parse_string(std::string_view text, std::size_t& pos, std::string& out);

    static bool parse_number(std::string_view text, std::size_t& pos, double& out) {
        const std::size_t start = pos;
        if (pos < text.size() && (text[pos] == '-' || text[pos] == '+')) ++pos;
        while (pos < text.size() && ((text[pos] >= '0' && text[pos] <= '9') || text[pos] == '.' || text[pos] == 'e' ||
                                     text[pos] == 'E' || text[pos] == '-' || text[pos] == '+')) {
            ++pos;
        }
        if (pos == start) return false;
        const auto slice = text.substr(start, pos - start);
        auto [ptr, ec] = std::from_chars(slice.data(), slice.data() + slice.size(), out);
        return ec == std::errc{} && ptr == slice.data() + slice.size();
    }

    Type type_{Type::Null};
    bool bool_{false};
    double number_{0.0};
    std::string string_;
    Array array_;
    Object object_;
};

// UTF-16 码位 → UTF-8（代理对等到此为止：原版数据文件基本不用 >U+FFFF）
inline void append_utf8(std::string& out, unsigned code) {
    if (code < 0x80U) {
        out.push_back(static_cast<char>(code));
    } else if (code < 0x800U) {
        out.push_back(static_cast<char>(0xC0U | (code >> 6U)));
        out.push_back(static_cast<char>(0x80U | (code & 0x3FU)));
    } else {
        out.push_back(static_cast<char>(0xE0U | (code >> 12U)));
        out.push_back(static_cast<char>(0x80U | ((code >> 6U) & 0x3FU)));
        out.push_back(static_cast<char>(0x80U | (code & 0x3FU)));
    }
}

inline bool JsonValue::parse_string(std::string_view text, std::size_t& pos, std::string& out) {
    if (pos >= text.size() || text[pos] != '"') return false;
    ++pos;
    out.clear();
    while (pos < text.size()) {
        const char c = text[pos++];
        if (c == '"') return true;
        if (c != '\\') {
            out.push_back(c);
            continue;
        }
        if (pos >= text.size()) return false;
        switch (text[pos++]) {
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case '/': out.push_back('/'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            case 'u': {
                if (pos + 4 > text.size()) return false;
                unsigned code = 0;
                for (int i = 0; i < 4; ++i) {
                    const char h = text[pos++];
                    code <<= 4U;
                    if (h >= '0' && h <= '9') code |= static_cast<unsigned>(h - '0');
                    else if (h >= 'a' && h <= 'f') code |= static_cast<unsigned>(h - 'a' + 10);
                    else if (h >= 'A' && h <= 'F') code |= static_cast<unsigned>(h - 'A' + 10);
                    else return false;
                }
                append_utf8(out, code);
                break;
            }
            default:
                return false;
        }
    }
    return false;
}

inline bool JsonValue::parse_value(std::string_view text, std::size_t& pos, JsonValue& out) {
    skip_ws(text, pos);
    if (pos >= text.size()) return false;

    switch (text[pos]) {
        case '{': {
            ++pos;
            Object object;
            skip_ws(text, pos);
            if (pos < text.size() && text[pos] == '}') {
                ++pos;
                out = make_object(std::move(object));
                return true;
            }
            while (true) {
                skip_ws(text, pos);
                std::string key;
                if (!parse_string(text, pos, key)) return false;
                skip_ws(text, pos);
                if (pos >= text.size() || text[pos] != ':') return false;
                ++pos;
                JsonValue value;
                if (!parse_value(text, pos, value)) return false;
                object.insert_or_assign(std::move(key), std::move(value));
                skip_ws(text, pos);
                if (pos >= text.size()) return false;
                if (text[pos] == ',') {
                    ++pos;
                    continue;
                }
                if (text[pos] == '}') {
                    ++pos;
                    out = make_object(std::move(object));
                    return true;
                }
                return false;
            }
        }
        case '[': {
            ++pos;
            Array array;
            skip_ws(text, pos);
            if (pos < text.size() && text[pos] == ']') {
                ++pos;
                out = make_array(std::move(array));
                return true;
            }
            while (true) {
                JsonValue value;
                if (!parse_value(text, pos, value)) return false;
                array.push_back(std::move(value));
                skip_ws(text, pos);
                if (pos >= text.size()) return false;
                if (text[pos] == ',') {
                    ++pos;
                    continue;
                }
                if (text[pos] == ']') {
                    ++pos;
                    out = make_array(std::move(array));
                    return true;
                }
                return false;
            }
        }
        case '"': {
            std::string value;
            if (!parse_string(text, pos, value)) return false;
            out = make_string(std::move(value));
            return true;
        }
        default:
            break;
    }

    if (text.compare(pos, 4, "true") == 0) {
        pos += 4;
        out = make_bool(true);
        return true;
    }
    if (text.compare(pos, 5, "false") == 0) {
        pos += 5;
        out = make_bool(false);
        return true;
    }
    if (text.compare(pos, 4, "null") == 0) {
        pos += 4;
        out = make_null();
        return true;
    }

    double number = 0.0;
    if (!parse_number(text, pos, number)) return false;
    out = make_number(number);
    return true;
}

}  // namespace cyane::core
