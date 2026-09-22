#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <variant>
#include <vector>

#include "cyane/core/error.hpp"

namespace cyane {

struct TransparentStringHash {
    using is_transparent = void;
    [[nodiscard]] std::size_t operator()(std::string_view key) const noexcept {
        return std::hash<std::string_view>{}(key);
    }
};

// TOML 子集：[section]、key = value，支持字符串/整数/浮点/布尔/字符串数组与 # 注释
class Config {
public:
    using Array = std::vector<std::string>;
    using Value = std::variant<std::string, std::int64_t, double, bool, Array>;
    using Map = std::unordered_map<std::string, Value, TransparentStringHash, std::equal_to<>>;

    [[nodiscard]] static Result<Config> load_file(const std::filesystem::path& path);
    [[nodiscard]] static Result<Config> parse(std::string_view text, std::string_view source = "<memory>");

    [[nodiscard]] bool contains(std::string_view key) const noexcept { return entries_.contains(key); }

    template <typename T>
    [[nodiscard]] std::optional<T> get(std::string_view key) const noexcept {
        const auto it = entries_.find(key);
        if (it == entries_.end()) {
            return std::nullopt;
        }
        return std::visit(
            []<typename V>(const V& value) -> std::optional<T> {
                if constexpr (std::is_same_v<V, T>) {
                    return value;
                } else if constexpr (std::is_integral_v<T> && !std::is_same_v<T, bool> &&
                                     std::is_same_v<V, std::int64_t>) {
                    return static_cast<T>(value);
                } else if constexpr (std::is_floating_point_v<T> &&
                                     (std::is_same_v<V, std::int64_t> || std::is_same_v<V, double>)) {
                    return static_cast<T>(value);
                } else {
                    return std::nullopt;
                }
            },
            it->second);
    }

    template <typename T>
    [[nodiscard]] T get_or(std::string_view key, T fallback) const noexcept {
        return get<T>(key).value_or(std::move(fallback));
    }

    [[nodiscard]] const Map& entries() const noexcept { return entries_; }

private:
    Map entries_;
};

}
