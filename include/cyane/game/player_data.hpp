#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include "cyane/core/error.hpp"
#include "cyane/core/uuid.hpp"
#include "cyane/item/item_stack.hpp"
#include "cyane/item/player_inventory.hpp"
#include "cyane/proto/json.hpp"
#include "cyane/proto/play_fields.hpp"

namespace cyane::game {

// 玩家持久化数据：位置、朝向、游戏模式、血量与 46 格背包
struct PlayerData {
    std::string username;
    std::string uuid_with_dashes;
    double x{0.5};
    double y{4.0};
    double z{0.5};
    float yaw{0.0f};
    float pitch{0.0f};
    std::uint8_t game_mode{proto::game_mode::kCreative};
    float health{20.0f};
    std::array<item::ItemStack, item::PlayerInventory::kSlotCount> inventory{};
};

namespace detail {

[[nodiscard]] inline std::string trim_copy(std::string_view text) {
    const auto first = text.find_first_not_of(" \t\r\n,");
    if (first == std::string_view::npos) {
        return {};
    }
    const auto last = text.find_last_not_of(" \t\r\n,");
    return std::string{text.substr(first, last - first + 1)};
}

[[nodiscard]] inline std::optional<std::string> unquote(std::string_view text) {
    const auto trimmed = trim_copy(text);
    if (trimmed.size() >= 2 && trimmed.front() == '"' && trimmed.back() == '"') {
        return std::string{trimmed.substr(1, trimmed.size() - 2)};
    }
    return std::nullopt;
}

} // namespace detail

// 极简 JSON 读写：只认我们自己写出的紧凑格式，避免引入外部依赖
class PlayerDataJson {
public:
    [[nodiscard]] static std::string serialize(const PlayerData& data) {
        std::string out;
        out.reserve(512);
        out += "{\n";
        out += "  \"username\": " + proto::json_string(data.username) + ",\n";
        out += "  \"uuid\": " + proto::json_string(data.uuid_with_dashes) + ",\n";
        out += "  \"x\": " + std::to_string(data.x) + ",\n";
        out += "  \"y\": " + std::to_string(data.y) + ",\n";
        out += "  \"z\": " + std::to_string(data.z) + ",\n";
        out += "  \"yaw\": " + std::to_string(data.yaw) + ",\n";
        out += "  \"pitch\": " + std::to_string(data.pitch) + ",\n";
        out += "  \"game_mode\": " + std::to_string(static_cast<int>(data.game_mode)) + ",\n";
        out += "  \"health\": " + std::to_string(data.health) + ",\n";
        out += "  \"inventory\": [";
        bool first = true;
        for (std::size_t index = 0; index < data.inventory.size(); ++index) {
            const auto& stack = data.inventory[index];
            if (stack.empty()) {
                continue;
            }
            if (!first) {
                out += ',';
            }
            out += "\n    {\"slot\": " + std::to_string(index) +
                   ", \"id\": " + std::to_string(stack.id) +
                   ", \"count\": " + std::to_string(static_cast<int>(stack.count)) +
                   ", \"damage\": " + std::to_string(stack.damage) + "}";
            first = false;
        }
        if (!first) {
            out += "\n  ";
        }
        out += "]\n}";
        return out;
    }

    [[nodiscard]] static std::optional<PlayerData> deserialize(std::string_view text) {
        PlayerData data;
        bool in_inventory = false;
        std::size_t cursor = 0;

        while (cursor <= text.size()) {
            const auto newline = text.find('\n', cursor);
            const auto line_view = text.substr(cursor, newline == std::string_view::npos
                                                          ? std::string_view::npos
                                                          : newline - cursor);
            cursor = newline == std::string_view::npos ? text.size() + 1 : newline + 1;

            const auto line = detail::trim_copy(line_view);
            if (line.empty() || line == "{" || line == "}") {
                continue;
            }
            if (line == "\"inventory\": [") {
                in_inventory = true;
                continue;
            }
            if (in_inventory && line == "]") {
                in_inventory = false;
                continue;
            }
            if (in_inventory) {
                read_stack(line, data);
                continue;
            }
            const auto colon = line.find(':');
            if (colon == std::string::npos) {
                continue;
            }
            const auto key = detail::unquote(line.substr(0, colon));
            const auto value = detail::trim_copy(line.substr(colon + 1));
            if (!key) {
                continue;
            }
            if (*key == "username" || *key == "uuid") {
                if (const auto text_value = detail::unquote(value)) {
                    if (*key == "username") {
                        data.username = *text_value;
                    } else {
                        data.uuid_with_dashes = *text_value;
                    }
                }
            } else if (*key == "x") {
                data.x = std::stod(value);
            } else if (*key == "y") {
                data.y = std::stod(value);
            } else if (*key == "z") {
                data.z = std::stod(value);
            } else if (*key == "yaw") {
                data.yaw = std::stof(value);
            } else if (*key == "pitch") {
                data.pitch = std::stof(value);
            } else if (*key == "game_mode") {
                data.game_mode = static_cast<std::uint8_t>(std::stoi(value));
            } else if (*key == "health") {
                data.health = std::stof(value);
            }
        }
        return data;
    }

private:
    // 从 {"slot": N, "id": X, "count": Y, "damage": Z} 读出一个堆叠
    static void read_stack(const std::string& line, PlayerData& data) {
        const auto slot = read_int(line, "slot");
        const auto id = read_int(line, "id");
        if (!slot || !id) {
            return;
        }
        if (*slot < 0 || static_cast<std::size_t>(*slot) >= data.inventory.size()) {
            return;
        }
        const auto count = read_int(line, "count").value_or(0);
        const auto damage = read_int(line, "damage").value_or(0);
        item::ItemStack stack{static_cast<std::int16_t>(*id), static_cast<std::uint8_t>(count),
                               static_cast<std::int16_t>(damage)};
        if (!stack.empty()) {
            data.inventory[static_cast<std::size_t>(*slot)] = stack;
        }
    }

    [[nodiscard]] static std::optional<long> read_int(const std::string& line, std::string_view key) {
        const auto key_pos = line.find('"' + std::string{key} + '"');
        if (key_pos == std::string::npos) {
            return std::nullopt;
        }
        const auto colon = line.find(':', key_pos);
        if (colon == std::string::npos) {
            return std::nullopt;
        }
        const auto end = line.find_first_of(",}]", colon);
        const auto value = detail::trim_copy(line.substr(colon + 1, end - colon - 1));
        try {
            return std::stol(value);
        } catch (...) {
            return std::nullopt;
        }
    }
};

// 玩家数据持久化存储：每个 UUID 一个 JSON 文件
class PlayerDataStore {
public:
    PlayerDataStore() = default;
    PlayerDataStore(const PlayerDataStore&) = delete;
    PlayerDataStore& operator=(const PlayerDataStore&) = delete;

    [[nodiscard]] PlayerData load_or_default(std::string_view uuid_with_dashes,
                                             std::string_view username) const {
        const auto path = path_for(uuid_with_dashes);
        std::lock_guard<std::mutex> lock{mutex_};
        PlayerData data;
        data.uuid_with_dashes = std::string{uuid_with_dashes};
        data.username = std::string{username};
        if (std::ifstream file{path}; file) {
            std::ostringstream buffer;
            buffer << file.rdbuf();
            if (auto parsed = PlayerDataJson::deserialize(buffer.str())) {
                parsed->uuid_with_dashes = std::string{uuid_with_dashes};
                if (parsed->username.empty()) {
                    parsed->username = std::string{username};
                }
                return *parsed;
            }
        }
        return data;
    }

    [[nodiscard]] Result<void> save(const PlayerData& data) {
        const auto path = path_for(data.uuid_with_dashes);
        std::lock_guard<std::mutex> lock{mutex_};
        std::ofstream file{path, std::ios::trunc | std::ios::binary};
        if (!file) {
            return make_error(ErrorCode::io, "cannot open player data file: " + path.string());
        }
        file << PlayerDataJson::serialize(data);
        if (!file) {
            return make_error(ErrorCode::io, "cannot write player data file: " + path.string());
        }
        return {};
    }

    void remove(std::string_view uuid_with_dashes) {
        const auto path = path_for(uuid_with_dashes);
        std::lock_guard<std::mutex> lock{mutex_};
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }

    [[nodiscard]] std::vector<std::string> list_players() const {
        std::lock_guard<std::mutex> lock{mutex_};
        std::vector<std::string> out;
        std::error_code ec;
        if (!std::filesystem::exists(dir_, ec)) {
            return out;
        }
        for (const auto& entry : std::filesystem::directory_iterator{dir_, ec}) {
            if (entry.is_regular_file() && entry.path().extension() == ".json") {
                out.push_back(Uuid::parse(entry.path().stem().string()).dashed());
            }
        }
        return out;
    }

    void set_dir(std::string_view dir) {
        std::lock_guard<std::mutex> lock{mutex_};
        dir_ = std::string{dir};
        std::error_code ec;
        std::filesystem::create_directories(dir_, ec);
    }

    [[nodiscard]] const std::string& dir() const noexcept { return dir_; }

private:
    [[nodiscard]] std::filesystem::path path_for(std::string_view uuid_with_dashes) const {
        return std::filesystem::path{dir_} / (std::string{uuid_with_dashes} + ".json");
    }

    std::string dir_{"player_data"};
    mutable std::mutex mutex_;
};

}
