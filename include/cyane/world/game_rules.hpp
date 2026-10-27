#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>

namespace cyane::world {

// 1.12.2 原版游戏规则系统（GameRules 逐行转写）
class GameRules {
public:
    enum class ValueType {
        Boolean,
        Numerical,
        String
    };

    struct Value {
        std::string value;
        ValueType type{ValueType::String};

        [[nodiscard]] bool get_bool() const noexcept {
            return value == "true" || value == "1";
        }

        [[nodiscard]] int get_int() const noexcept {
            try {
                return std::stoi(value);
            } catch (...) {
                return 0;
            }
        }
    };

    GameRules() {
        add_rule("doFireTick", "true", ValueType::Boolean);
        add_rule("mobGriefing", "true", ValueType::Boolean);
        add_rule("keepInventory", "false", ValueType::Boolean);
        add_rule("doMobSpawning", "true", ValueType::Boolean);
        add_rule("doMobLoot", "true", ValueType::Boolean);
        add_rule("doTileDrops", "true", ValueType::Boolean);
        add_rule("doEntityDrops", "true", ValueType::Boolean);
        add_rule("commandBlockOutput", "true", ValueType::Boolean);
        add_rule("naturalRegeneration", "true", ValueType::Boolean);
        add_rule("doDaylightCycle", "true", ValueType::Boolean);
        add_rule("logAdminCommands", "true", ValueType::Boolean);
        add_rule("showDeathMessages", "true", ValueType::Boolean);
        add_rule("randomTickSpeed", "3", ValueType::Numerical);
        add_rule("sendCommandFeedback", "true", ValueType::Boolean);
        add_rule("reducedDebugInfo", "false", ValueType::Boolean);
        add_rule("spectatorsGenerateChunks", "true", ValueType::Boolean);
        add_rule("spawnRadius", "10", ValueType::Numerical);
        add_rule("disableElytraMovementCheck", "false", ValueType::Boolean);
        add_rule("maxEntityCramming", "24", ValueType::Numerical);
        add_rule("doWeatherCycle", "true", ValueType::Boolean);
        add_rule("doLimitedCrafting", "false", ValueType::Boolean);
        add_rule("maxCommandChainLength", "65536", ValueType::Numerical);
        add_rule("announceAdvancements", "true", ValueType::Boolean);
        add_rule("gameLoopFunction", "-", ValueType::String);
    }

    void add_rule(std::string_view key, std::string_view val, ValueType type) {
        rules_[std::string(key)] = Value{std::string(val), type};
    }

    void set_or_create(std::string_view key, std::string_view val) {
        auto it = rules_.find(std::string(key));
        if (it != rules_.end()) {
            it->second.value = std::string(val);
        } else {
            add_rule(key, val, ValueType::String);
        }
    }

    [[nodiscard]] bool get_bool(std::string_view key) const noexcept {
        auto it = rules_.find(std::string(key));
        if (it != rules_.end()) {
            return it->second.get_bool();
        }
        return false;
    }

    [[nodiscard]] int get_int(std::string_view key) const noexcept {
        auto it = rules_.find(std::string(key));
        if (it != rules_.end()) {
            return it->second.get_int();
        }
        return 0;
    }

    [[nodiscard]] std::string_view get_string(std::string_view key) const noexcept {
        auto it = rules_.find(std::string(key));
        if (it != rules_.end()) {
            return it->second.value;
        }
        return "";
    }

    [[nodiscard]] bool has_rule(std::string_view key) const noexcept {
        return rules_.contains(std::string(key));
    }

    [[nodiscard]] const std::map<std::string, Value>& get_rules() const noexcept {
        return rules_;
    }

private:
    std::map<std::string, Value> rules_;
};

}  // namespace cyane::world
