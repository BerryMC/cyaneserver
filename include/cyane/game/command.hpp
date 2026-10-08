#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "cyane/entity/player_entity.hpp"

namespace cyane {
class Server;
}

namespace cyane::game {

class CommandSender {
public:
    virtual ~CommandSender() = default;
    [[nodiscard]] virtual std::string_view name() const noexcept = 0;
    [[nodiscard]] virtual bool is_player() const noexcept = 0;
    [[nodiscard]] virtual std::uint8_t op_level() const noexcept = 0;
    [[nodiscard]] virtual const entity::Position* player_position() const noexcept { return nullptr; }
    [[nodiscard]] virtual std::uint32_t player_entity_id() const noexcept { return 0; }
    virtual void send_feedback(std::string_view message, bool is_error = false) = 0;
};

class CommandDispatcher {
public:
    CommandDispatcher() = default;

    // 执行一条命令，返回 false 表示收到 stop 命令要求停止服务器
    static bool execute(CommandSender& sender, Server& server, std::string_view text);

    // 获取 Tab 补全候选词
    [[nodiscard]] static std::vector<std::string> tab_complete(const CommandSender& sender,
                                                               const Server& server,
                                                               std::string_view text);
};

}  // namespace cyane::game
