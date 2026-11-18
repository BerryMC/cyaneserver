#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string_view>

#include "cyane/core/error.hpp"
#include "cyane/world/world_info.hpp"

namespace cyane::world::storage {

// 1.12.2 的 session.lock 是大端时间戳，不是操作系统文件锁。
class SaveHandler {
public:
    SaveHandler() = default;
    SaveHandler(const SaveHandler&) = delete;
    SaveHandler& operator=(const SaveHandler&) = delete;

    [[nodiscard]] Result<void> open(const std::filesystem::path& world_dir,
                                    std::optional<std::int64_t> initialization_time_ms = std::nullopt,
                                    bool store_player_data = true);
    [[nodiscard]] Result<void> check_session_lock() const;
    [[nodiscard]] Result<WorldInfo> load_world_info() const;
    [[nodiscard]] Result<void> save_world_info(const WorldInfo& info) const;

    // 原版关服不删除 session.lock；下次打开世界时覆盖旧时间戳。
    void close() noexcept { initialized_ = false; }

    [[nodiscard]] const std::filesystem::path& world_dir() const noexcept { return world_dir_; }
    [[nodiscard]] std::filesystem::path level_dat() const { return world_dir_ / "level.dat"; }
    [[nodiscard]] std::filesystem::path level_dat_old() const { return world_dir_ / "level.dat_old"; }
    [[nodiscard]] std::filesystem::path region_dir() const { return world_dir_ / "region"; }
    [[nodiscard]] std::filesystem::path player_dir() const { return world_dir_ / "playerdata"; }
    [[nodiscard]] std::filesystem::path data_dir() const { return world_dir_ / "data"; }
    [[nodiscard]] std::filesystem::path session_lock_path() const { return world_dir_ / "session.lock"; }
    [[nodiscard]] std::filesystem::path map_file(std::string_view name) const;
    [[nodiscard]] std::int64_t initialization_time() const noexcept { return initialization_time_; }
    [[nodiscard]] bool is_open() const noexcept { return initialized_; }
    [[nodiscard]] static std::int64_t now_ms() noexcept;

private:
    std::filesystem::path world_dir_;
    std::int64_t initialization_time_{0};
    bool initialized_{false};
};

}  // namespace cyane::world::storage
