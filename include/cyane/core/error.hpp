#pragma once

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace cyane {

enum class ErrorCode : std::uint8_t {
    io,
    net,
    protocol,
    world,
    config,
    plugin,
    jvm,
    internal,
};

[[nodiscard]] constexpr std::string_view to_string(ErrorCode code) noexcept {
    switch (code) {
        case ErrorCode::io: return "io";
        case ErrorCode::net: return "net";
        case ErrorCode::protocol: return "protocol";
        case ErrorCode::world: return "world";
        case ErrorCode::config: return "config";
        case ErrorCode::plugin: return "plugin";
        case ErrorCode::jvm: return "jvm";
        case ErrorCode::internal: return "internal";
    }
    return "unknown";
}

struct Error {
    ErrorCode code{ErrorCode::internal};
    std::string message;

    Error(ErrorCode c, std::string msg) noexcept : code{c}, message{std::move(msg)} {}
};

template <typename T = void>
using Result = std::conditional_t<std::is_void_v<T>, std::expected<void, Error>, std::expected<T, Error>>;

[[nodiscard]] inline std::unexpected<Error> make_error(ErrorCode code, std::string message) {
    return std::unexpected{Error{code, std::move(message)}};
}

}
