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

#include "cyane/core/json_parse.hpp"
#include "cyane/core/uuid.hpp"

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

}  // namespace

// 兼容两种格式：
//   1) 原版 ops.json 数组：[{"uuid":"...","name":"...","level":4,"bypassesPlayerLimit":false}, ...]
//   2) 旧版映射：{"<uuid>": {"name": "...", "level": 4}}
Result<void> OpManager::load(std::string_view path) {
    path_ = std::string{path};
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        const auto parent = std::filesystem::path{path}.parent_path();
        if (!parent.empty()) std::filesystem::create_directories(parent, ec);
        std::ofstream out{path_};
        if (!out) {
            return make_error(ErrorCode::config, "cannot create ops file");
        }
        out << "[]\n";
        return {};
    }

    const auto content = read_file(path);
    if (!content) {
        return make_error(ErrorCode::config, "cannot read ops file");
    }

    const auto parsed = core::JsonValue::parse(*content);
    if (!parsed) {
        return make_error(ErrorCode::config, "ops.json: invalid JSON");
    }

    std::lock_guard<std::mutex> lock(mutex_);
    levels_.clear();
    uuids_.clear();
    display_names_.clear();

    const auto register_entry = [this](std::string uuid, std::string name, std::int64_t level) {
        std::string key = Uuid::parse(uuid).dashed();
        std::transform(key.begin(), key.end(), key.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        const auto clamped = static_cast<std::uint8_t>(level < 1 ? 1 : (level > 4 ? 4 : level));
        levels_[key] = clamped;
        const auto original = name;
        std::transform(name.begin(), name.end(), name.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (!name.empty()) uuids_[name] = key;
        display_names_[key] = original;
    };

    if (parsed->is_array()) {
        for (const auto& entry : parsed->as_array()) {
            if (!entry.is_object()) continue;
            const auto uuid = entry.get_string("uuid", "");
            if (uuid.empty()) continue;
            register_entry(std::string{uuid}, std::string{entry.get_string("name", "")},
                           entry.get_int("level", 0));
        }
        return {};
    }

    if (!parsed->is_object()) {
        return {};
    }

    for (const auto& [uuid, value] : parsed->as_object()) {
        if (!value.is_object()) continue;
        register_entry(uuid, std::string{value.get_string("name", "")}, value.get_int("level", 0));
    }
    return {};
}

void OpManager::save() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    std::ofstream out(path_, std::ios::trunc);
    if (!out) return;
    // 原版格式：数组，每项 {uuid,name,level,bypassesPlayerLimit}
    out << "[\n";
    std::size_t i = 0;
    for (const auto& [uuid, level] : levels_) {
        std::string name;
        if (const auto it = display_names_.find(uuid); it != display_names_.end()) {
            name = it->second;
        } else {
            for (const auto& [n, u] : uuids_) {
                if (u == uuid) { name = n; break; }
            }
        }
        std::string escaped;
        for (char c : name) {
            if (c == '"' || c == '\\') escaped.push_back('\\');
            escaped.push_back(c);
        }
        out << "  {\"uuid\": \"" << uuid << "\", \"name\": \"" << escaped
            << "\", \"level\": " << static_cast<int>(level) << ", \"bypassesPlayerLimit\": false}";
        if (i + 1 < levels_.size()) out << ",";
        out << "\n";
        ++i;
    }
    out << "]\n";
}

bool OpManager::op_player(std::string_view uuid, std::string_view name, std::uint8_t level) {
    std::string u = Uuid::parse(uuid).dashed();
    std::transform(u.begin(), u.end(), u.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::string n{name};
    std::transform(n.begin(), n.end(), n.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (level < 1) level = 1;
    if (level > 4) level = 4;
    bool new_op = true;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        new_op = levels_.find(u) == levels_.end();
        levels_[u] = level;
        uuids_[n] = u;
        display_names_[u] = std::string{name};
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
        display_names_.erase(u);
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
