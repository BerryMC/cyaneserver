#include "cyane/core/config.hpp"

#include <charconv>
#include <format>
#include <fstream>
#include <sstream>

namespace cyane {
namespace {

constexpr std::string_view kWhitespace = " \t\r\n";

[[nodiscard]] std::string_view trim(std::string_view text) noexcept {
    const auto first = text.find_first_not_of(kWhitespace);
    if (first == std::string_view::npos) {
        return {};
    }
    return text.substr(first, text.find_last_not_of(kWhitespace) - first + 1);
}

[[nodiscard]] std::string_view strip_comment(std::string_view text) noexcept {
    bool in_string = false;
    char quote = '\0';
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (in_string) {
            if (c == '\\' && quote == '"') {
                ++i;
            } else if (c == quote) {
                in_string = false;
            }
            continue;
        }
        if (c == '"' || c == '\'') {
            in_string = true;
            quote = c;
        } else if (c == '#') {
            return trim(text.substr(0, i));
        }
    }
    return trim(text);
}

[[nodiscard]] Result<std::string> parse_escaped(std::string_view raw, std::string_view where) {
    std::string out;
    out.reserve(raw.size());
    for (std::size_t i = 1; i < raw.size(); ++i) {
        const char c = raw[i];
        if (c == '\\') {
            if (i + 1 >= raw.size()) {
                break;
            }
            const char escaped = raw[++i];
            switch (escaped) {
                case 'n': out.push_back('\n'); break;
                case 't': out.push_back('\t'); break;
                case 'r': out.push_back('\r'); break;
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                default:
                    return make_error(ErrorCode::config,
                                             std::format("{}: unknown escape '\\{}'", where, escaped));
            }
        } else if (c == '"') {
            if (!trim(raw.substr(i + 1)).empty()) {
                return make_error(ErrorCode::config,
                                         std::format("{}: trailing characters after string literal", where));
            }
            return out;
        } else {
            out.push_back(c);
        }
    }
    return make_error(ErrorCode::config, std::format("{}: unterminated string", where));
}

[[nodiscard]] Result<std::string> parse_literal(std::string_view raw, std::string_view where) {
    if (raw.size() < 2 || raw.find('\'', 1) != raw.size() - 1) {
        return make_error(ErrorCode::config,
                                 std::format("{}: unterminated literal string {}", where, raw));
    }
    return std::string{raw.substr(1, raw.size() - 2)};
}

[[nodiscard]] Result<Config::Array> parse_array(std::string_view raw, std::string_view where) {
    std::string_view body = trim(raw.substr(1));
    if (body.empty() || body.back() != ']') {
        return make_error(ErrorCode::config, std::format("{}: unterminated array", where));
    }
    body.remove_suffix(1);

    Config::Array items;
    std::size_t start = 0;
    bool in_string = false;
    char quote = '\0';
    for (std::size_t i = 0; i <= body.size(); ++i) {
        const bool at_end = i == body.size();
        const char c = at_end ? ',' : body[i];
        if (in_string) {
            if (c == '\\' && quote == '"' && !at_end) {
                ++i;
            } else if (c == quote) {
                in_string = false;
            }
            continue;
        }
        if (!at_end && (c == '"' || c == '\'')) {
            in_string = true;
            quote = c;
            continue;
        }
        if (c != ',') {
            continue;
        }
        const auto item = trim(body.substr(start, i - start));
        start = i + 1;
        if (item.empty()) {
            continue;
        }
        if (item.front() == '"' || item.front() == '\'') {
            auto parsed = item.front() == '"' ? parse_escaped(item, where) : parse_literal(item, where);
            if (!parsed) {
                return std::unexpected{std::move(parsed.error())};
            }
            items.push_back(std::move(*parsed));
        } else {
            items.emplace_back(item);
        }
    }
    if (in_string) {
        return make_error(ErrorCode::config, std::format("{}: unterminated string in array", where));
    }
    return items;
}

[[nodiscard]] Result<Config::Value> parse_value(std::string_view raw, std::string_view where) {
    if (raw.empty()) {
        return make_error(ErrorCode::config, std::format("{}: missing value", where));
    }
    switch (raw.front()) {
        case '"': {
            auto text = parse_escaped(raw, where);
            if (!text) {
                return std::unexpected{std::move(text.error())};
            }
            return Config::Value{std::move(*text)};
        }
        case '\'': {
            auto text = parse_literal(raw, where);
            if (!text) {
                return std::unexpected{std::move(text.error())};
            }
            return Config::Value{std::move(*text)};
        }
        case '[': {
            auto array = parse_array(raw, where);
            if (!array) {
                return std::unexpected{std::move(array.error())};
            }
            return Config::Value{std::move(*array)};
        }
        default: break;
    }
    if (raw == "true") {
        return Config::Value{true};
    }
    if (raw == "false") {
        return Config::Value{false};
    }
    const char* const begin = raw.data();
    const char* const end = begin + raw.size();
    std::int64_t integer = 0;
    if (auto [ptr, ec] = std::from_chars(begin, end, integer); ec == std::errc{} && ptr == end) {
        return Config::Value{integer};
    }
    double floating = 0.0;
    if (auto [ptr, ec] = std::from_chars(begin, end, floating); ec == std::errc{} && ptr == end) {
        return Config::Value{floating};
    }
    return make_error(ErrorCode::config, std::format("{}: unrecognized value '{}'", where, raw));
}

}

Result<Config> Config::parse(std::string_view text, std::string_view source) {
    Config config;
    std::string section;
    std::size_t line_number = 0;

    for (std::size_t pos = 0; pos <= text.size();) {
        const auto newline = text.find('\n', pos);
        auto line = trim(text.substr(pos, newline == std::string_view::npos ? newline : newline - pos));
        pos = newline == std::string_view::npos ? text.size() + 1 : newline + 1;
        ++line_number;

        if (line.empty() || line.front() == '#') {
            continue;
        }
        const auto where = std::format("{}:{}", source, line_number);

        if (line.front() == '[') {
            if (line.back() != ']') {
                return make_error(ErrorCode::config, std::format("{}: malformed section header", where));
            }
            section.assign(trim(line.substr(1, line.size() - 2)));
            continue;
        }

        const auto equals = line.find('=');
        if (equals == std::string_view::npos) {
            return make_error(ErrorCode::config, std::format("{}: expected 'key = value'", where));
        }
        auto key = trim(line.substr(0, equals));
        if (key.empty()) {
            return make_error(ErrorCode::config, std::format("{}: empty key", where));
        }
        if (key.size() >= 2 && key.front() == '"' && key.back() == '"') {
            key = key.substr(1, key.size() - 2);
        }

        auto value = parse_value(strip_comment(line.substr(equals + 1)), where);
        if (!value) {
            return std::unexpected{std::move(value.error())};
        }

        std::string full_key = section.empty() ? std::string{key} : std::format("{}.{}", section, key);
        config.entries_.insert_or_assign(std::move(full_key), std::move(*value));
    }
    return config;
}

Result<Config> Config::load_file(const std::filesystem::path& path) {
    std::ifstream file{path, std::ios::binary};
    if (!file) {
        return make_error(ErrorCode::config, std::format("cannot open {}", path.string()));
    }
    std::ostringstream contents;
    contents << file.rdbuf();
    const std::string text = std::move(contents).str();
    return parse(text, path.string());
}

}
