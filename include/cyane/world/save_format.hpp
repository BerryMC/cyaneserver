#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "cyane/core/error.hpp"
#include "cyane/world/save_handler.hpp"
#include "cyane/world/world_info.hpp"

namespace cyane::world::storage {

// 1.12.2 原版旧存档格式管理器（SaveFormatOld / ISaveFormat）。
class SaveFormatOld {
public:
    explicit SaveFormatOld(std::filesystem::path saves_directory);

    [[nodiscard]] static constexpr std::string_view name() noexcept { return "Old Format"; }
    [[nodiscard]] const std::filesystem::path& saves_directory() const noexcept { return saves_directory_; }

    [[nodiscard]] Result<std::optional<WorldInfo>> get_world_info(std::string_view save_name) const;
    [[nodiscard]] Result<std::vector<WorldSummary>> get_save_list() const;
    [[nodiscard]] Result<std::unique_ptr<SaveHandler>> get_save_loader(
        std::string_view save_name, bool store_player_data = true) const;

    [[nodiscard]] bool is_new_level_id_acceptable(std::string_view save_name) const;
    [[nodiscard]] bool delete_world_directory(std::string_view save_name) const;
    [[nodiscard]] Result<void> rename_world(std::string_view directory_name,
                                            std::string_view new_level_name) const;
    [[nodiscard]] bool can_load_world(std::string_view save_name) const;
    [[nodiscard]] std::filesystem::path get_file(std::string_view save_name,
                                                 std::string_view file_name) const;

    [[nodiscard]] static constexpr bool is_convertible(std::string_view) noexcept { return false; }
    [[nodiscard]] static constexpr bool is_old_map_format(std::string_view) noexcept { return false; }
    static constexpr void flush_cache() noexcept {}

private:
    std::filesystem::path saves_directory_;
};

}  // namespace cyane::world::storage
