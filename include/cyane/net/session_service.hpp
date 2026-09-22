#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <string_view>

namespace cyane::net {

// 在线验证：确认客户端会话有效，成功返回无连字符 UUID
class SessionService {
public:
    virtual ~SessionService() = default;

    [[nodiscard]] virtual std::optional<std::string> has_joined(
        std::string_view username, std::string_view server_id, std::string_view ip) const = 0;
};

class MojangSessionService final : public SessionService {
public:
    std::chrono::milliseconds timeout{std::chrono::seconds{5}};

    [[nodiscard]] std::optional<std::string> has_joined(
        std::string_view username, std::string_view server_id, std::string_view ip) const override;
};

}
