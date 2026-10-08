#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "cyane/core/error.hpp"

namespace cyane::game {

// OP 权限管理器：持久化存储 ops.json，管理 UUID → 等级映射。
// 等级 1-4，4 为最高（服务器所有者）。op_level >= 2 可执行大多数管理命令。
class OpManager {
public:
    OpManager() = default;

    // 加载 ops.json，若文件不存在则创建空文件
    [[nodiscard]] Result<void> load(std::string_view path);

    // 保存到 ops.json
    void save() const noexcept;

    // 查询玩家 OP 等级（0 表示非 OP）
    [[nodiscard]] std::uint8_t op_level(std::string_view uuid) const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        if (const auto it = levels_.find(std::string{uuid}); it != levels_.end()) {
            return it->second;
        }
        return 0;
    }

    [[nodiscard]] bool is_op(std::string_view uuid) const noexcept {
        return op_level(uuid) > 0;
    }

    // 添加 OP（需要玩家名，用于反查 UUID，原版默认等级 4）
    [[nodiscard]] bool op_player(std::string_view uuid, std::string_view name, std::uint8_t level = 4);

    // 降级/移除 OP
    [[nodiscard]] bool deop_player(std::string_view uuid);

    // 玩家名 → UUID 反查（用于 /op <name> 命令）
    [[nodiscard]] std::optional<std::string> uuid_by_name(std::string_view name) const noexcept;

    // 获取所有 OP 的 UUID 列表
    [[nodiscard]] std::vector<std::string> all_uuids() const;

private:
    std::string path_;
    std::unordered_map<std::string, std::uint8_t> levels_;   // uuid -> level
    std::unordered_map<std::string, std::string> uuids_;    // name_lower -> uuid
    mutable std::mutex mutex_;
};

}
