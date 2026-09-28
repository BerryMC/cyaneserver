#include "cyane/game/op_manager.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <system_error>

namespace cyane::game {

namespace {
// 读取整个文件
[[nodiscard]] std::optional<std::string> read_file(std::string_view path) {
    std::ifstream in(path.data());
    if (!in) {
        return std::nullopt;
    }
    std::stringstream buf;
    buf << in.rdbuf();
    return buf.str();
}

// 跳过空白
void skip_ws(std::string_view& s) {
    while (!s.empty() && (s[0] == ' ' || s[0] == '\t' || s[0] == '\r' || s[0] == '\n')) {
        s.remove_prefix(1);
    }
}

// 解析 JSON 字符串（不处理转义序列，仅处理基本 ASCII）
[[nodiscard]] std::optional<std::string> parse_json_string(std::string_view& s) {
    skip_ws(s);
    if (s.empty() || s[0] != '"') {
        return std::nullopt;
    }
    s.remove_prefix(1);
    std::string result;
    while (!s.empty() && s[0] != '"') {
        if (s[0] == '\\') {
            s.remove_prefix(1);
            if (s.empty()) return std::nullopt;
            switch (s[0]) {
                case '"': result += '"'; break;
                case '\\': result += '\\'; break;
                case '/': result += '/'; break;
                case 'n': result += '\n'; break;
                case 't': result += '\t'; break;
                case 'r': result += '\r'; break;
                default: result += '\\'; result += s[0]; break;
            }
            s.remove_prefix(1);
        } else {
            result += s[0];
            s.remove_prefix(1);
        }
    }
    if (s.empty()) return std::nullopt;
    s.remove_prefix(1); // consume closing quote
    return result;
}

    // 解析 JSON 数字（int）
    [[nodiscard]] std::optional<std::int64_t> parse_json_number(std::string_view& s) {
        skip_ws(s);
        size_t pos = 0;
        while (pos < s.size() && (s[pos] == '-' || (s[pos] >= '0' && s[pos] <= '9'))) {
            ++pos;
        }
        if (pos == 0) return std::nullopt;
        std::int64_t val = 0;
        const auto [_, ec] = std::from_chars(s.data(), s.data() + pos, val);
        if (ec != std::errc{}) return std::nullopt;
        s.remove_prefix(pos);
        return val;
    }

    // 跳过 JSON 值（用于跳过未知字段）
    void skip_json_value(std::string_view& s) {
        skip_ws(s);
        if (s.empty()) return;
        if (s[0] == '"') { (void)parse_json_string(s); return; }
        if (s[0] == '-' || (s[0] >= '0' && s[0] <= '9')) { (void)parse_json_number(s); return; }
        if (s[0] == '{') {
            s.remove_prefix(1);
            while (!s.empty() && s[0] != '}') {
                skip_json_value(s);
                skip_ws(s);
                if (!s.empty() && s[0] == ',') s.remove_prefix(1);
            }
            if (!s.empty()) s.remove_prefix(1);
            return;
        }
        if (s[0] == '[') {
            s.remove_prefix(1);
            while (!s.empty() && s[0] != ']') {
                skip_json_value(s);
                skip_ws(s);
                if (!s.empty() && s[0] == ',') s.remove_prefix(1);
            }
            if (!s.empty()) s.remove_prefix(1);
            return;
        }
        // true/false/null
        while (!s.empty() && s[0] != ',' && s[0] != '}' && s[0] != ']') s.remove_prefix(1);
    }

}

Result<void> OpManager::load(std::string_view path) {
    path_ = std::string(path);
    const auto content = read_file(path_);
    if (!content.has_value()) {
        // 文件不存在：创建空文件
        std::ofstream out(path_.data(), std::ios::trunc);
        if (!out) {
            return make_error(ErrorCode::config, "cannot create ops file");
        }
        out << "{}\n";
        return {};
    }
    std::string_view s{content.value()};
    skip_ws(s);
    if (s.empty() || s[0] != '{') {
        // 空文件或格式错误：重置
        levels_.clear();
        uuids_.clear();
        return {};
    }
    s.remove_prefix(1); // consume '{'
    while (!s.empty()) {
        skip_ws(s);
        if (s.empty() || s[0] == '}') break;
        // 解析 UUID（key）
        const auto uuid_opt = parse_json_string(s);
        if (!uuid_opt) return make_error(ErrorCode::config, "ops.json: expected UUID key");
        const std::string& uuid = uuid_opt.value();
        skip_ws(s);
        if (s.empty() || s[0] != ':') break;
        s.remove_prefix(1);
        // 解析对象 { "name": "...", "level": N }
        skip_ws(s);
        if (s.empty() || s[0] != '{') break;
        s.remove_prefix(1);
        std::string name;
        std::uint8_t level = 0;
        bool has_name = false, has_level = false;
        while (!s.empty()) {
            skip_ws(s);
            if (s.empty() || s[0] == '}') break;
            const auto key_opt = parse_json_string(s);
            if (!key_opt) break;
            const std::string& key = key_opt.value();
            skip_ws(s);
            if (s.empty() || s[0] != ':') break;
            s.remove_prefix(1);
            if (key == "name") {
                const auto val = parse_json_string(s);
                if (val) { name = val.value(); has_name = true; }
            } else if (key == "level") {
                const auto val = parse_json_number(s);
                if (val.has_value()) {
                    level = static_cast<std::uint8_t>(*val > 4 ? 4 : *val);
                    has_level = true;
                }
            } else {
                // 跳过未知值
                if (s[0] == '"') { (void)parse_json_string(s); }
                else if (s[0] == '-' || (s[0] >= '0' && s[0] <= '9')) { (void)parse_json_number(s); }
                else if (s[0] == '{') { skip_json_value(s); }
            }
            skip_ws(s);
            if (!s.empty() && s[0] == ',') s.remove_prefix(1);
        }
        if (s.empty() || s[0] != '}') break;
        s.remove_prefix(1);
        if (has_name && has_level) {
            levels_[uuid] = level;
            // 名称不区分大小写存储
            std::string lower_name = name;
            std::transform(lower_name.begin(), lower_name.end(), lower_name.begin(),
                           [](unsigned char c) { return std::tolower(c); });
            uuids_[lower_name] = uuid;
        }
        skip_ws(s);
        if (!s.empty() && s[0] == ',') s.remove_prefix(1);
    }
    return {};
}

void OpManager::save() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    std::ofstream out(path_, std::ios::trunc);
    if (!out) return;
    out << "{\n";
    std::size_t i = 0;
    for (const auto& [uuid, level] : levels_) {
        out << "  \"" << uuid << "\": { \"name\": \"";
        // 反查名称
        std::string name;
        for (const auto& [n, u] : uuids_) {
            if (u == uuid) { name = n; break; }
        }
        for (char c : name) {
            if (c == '"') out << "\\\"";
            else if (c == '\\') out << "\\\\";
            else out << c;
        }
        out << "\", \"level\": " << static_cast<int>(level) << " }";
        if (i + 1 < levels_.size()) out << ",";
        out << "\n";
        ++i;
    }
    out << "}\n";
}

bool OpManager::op_player(std::string_view uuid, std::string_view name, std::uint8_t level) {
    const std::string u{uuid};
    std::string n{name};
    std::transform(n.begin(), n.end(), n.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    if (level < 1) level = 1;
    if (level > 4) level = 4;
    bool new_op = true;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        new_op = levels_.find(u) == levels_.end();
        levels_[u] = level;
        uuids_[n] = u;
    }
    save();
    return new_op;
}

bool OpManager::deop_player(std::string_view uuid) {
    const std::string u{uuid};
    bool found = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto li = levels_.find(u);
        if (li == levels_.end()) return false;
        found = true;
        // uuids_ 是 name_lower → uuid，需要反向查找删除
        for (auto it = uuids_.begin(); it != uuids_.end(); ++it) {
            if (it->second == u) {
                uuids_.erase(it);
                break;
            }
        }
        levels_.erase(li);
    }
    if (found) save();
    return found;
}

std::optional<std::string> OpManager::uuid_by_name(std::string_view name) const noexcept {
    std::string lower_name = std::string{name};
    std::transform(lower_name.begin(), lower_name.end(), lower_name.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = uuids_.find(lower_name);
    if (it != uuids_.end()) {
        return it->second;
    }
    return std::nullopt;
}

std::vector<std::string> OpManager::all_uuids() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> out;
    out.reserve(levels_.size());
    for (const auto& [uuid, _] : levels_) {
        out.push_back(uuid);
    }
    return out;
}

}
