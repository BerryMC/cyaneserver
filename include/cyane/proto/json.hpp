#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace cyane::proto {

inline void append_json_string(std::string& out, std::string_view text) {
    out.push_back('"');
    for (const char raw : text) {
        const auto byte = static_cast<std::uint8_t>(raw);
        switch (raw) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            default:
                if (byte < 0x20) {
                    constexpr char kHex[] = "0123456789abcdef";
                    out += "\\u00";
                    out.push_back(kHex[(byte >> 4) & 0x0F]);
                    out.push_back(kHex[byte & 0x0F]);
                } else {
                    out.push_back(raw);
                }
        }
    }
    out.push_back('"');
}

[[nodiscard]] inline std::string json_string(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 2);
    append_json_string(out, text);
    return out;
}

// 1.12.2 的聊天/断开原因均为聊天组件 JSON
[[nodiscard]] inline std::string chat_text(std::string_view text) {
    std::string out{"{\"text\":"};
    append_json_string(out, text);
    out.push_back('}');
    return out;
}

}
