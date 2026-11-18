#include "cyane/world/save_format.hpp"

#include <chrono>
#include <fstream>
#include <iterator>
#include <thread>

#include "cyane/proto/frame.hpp"
#include "cyane/world/level_dat.hpp"
#include "cyane/world/nbt.hpp"

namespace cyane::world::storage {

SaveFormatOld::SaveFormatOld(std::filesystem::path saves_directory)
    : saves_directory_{std::move(saves_directory)} {
    std::error_code ec;
    std::filesystem::create_directories(saves_directory_, ec);
}

Result<std::optional<WorldInfo>> SaveFormatOld::get_world_info(std::string_view save_name) const {
    const auto directory = saves_directory_ / save_name;
    if (!std::filesystem::exists(directory)) return std::optional<WorldInfo>{};
    auto info = world::load_world_info(directory);
    if (!info) return std::optional<WorldInfo>{};
    return std::optional<WorldInfo>{std::move(*info)};
}

Result<std::vector<WorldSummary>> SaveFormatOld::get_save_list() const {
    std::vector<WorldSummary> summaries;
    // SaveFormatOld 是 pre-Anvil 的旧格式，只枚举固定 World1..World5。
    for (int i = 1; i <= 5; ++i) {
        const auto directory_name = "World" + std::to_string(i);
        auto info = get_world_info(directory_name);
        if (!info) return std::unexpected{std::move(info.error())};
        if (!info->has_value()) continue;
        WorldSummary summary;
        summary.random_seed = (*info)->random_seed;
        summary.level_name = (*info)->level_name;
        summary.size_on_disk = (*info)->size_on_disk;
        summary.last_played = (*info)->last_played;
        summary.version_name = (*info)->version_name;
        summary.version_id = (*info)->version_id;
        summary.requires_conversion = false;
        summaries.push_back(std::move(summary));
    }
    return summaries;
}

Result<std::unique_ptr<SaveHandler>> SaveFormatOld::get_save_loader(
    std::string_view save_name, bool store_player_data) const {
    auto handler = std::make_unique<SaveHandler>();
    if (auto opened = handler->open(saves_directory_ / save_name, std::nullopt, store_player_data);
        !opened) {
        return std::unexpected{std::move(opened.error())};
    }
    return handler;
}

bool SaveFormatOld::is_new_level_id_acceptable(std::string_view save_name) const {
    const auto directory = saves_directory_ / save_name;
    if (std::filesystem::exists(directory)) return false;
    try {
        std::filesystem::create_directory(directory);
        std::filesystem::remove(directory);
        return true;
    } catch (const std::filesystem::filesystem_error&) {
        return false;
    }
}

bool SaveFormatOld::delete_world_directory(std::string_view save_name) const {
    const auto directory = saves_directory_ / save_name;
    if (!std::filesystem::exists(directory)) return true;
    // 原版最多尝试 5 次，每次失败等待 500ms。
    for (int attempt = 0; attempt < 5; ++attempt) {
        std::error_code ec;
        std::filesystem::remove_all(directory, ec);
        if (!ec && !std::filesystem::exists(directory)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds{500});
    }
    return false;
}

Result<void> SaveFormatOld::rename_world(std::string_view directory_name,
                                         std::string_view new_level_name) const {
    const auto directory = saves_directory_ / directory_name;
    if (!std::filesystem::exists(directory)) {
        return make_error(ErrorCode::io, "world directory does not exist");
    }
    const auto path = directory / "level.dat";
    if (!std::filesystem::exists(path)) return {};
    std::ifstream in{path, std::ios::binary};
    if (!in) return make_error(ErrorCode::io, "cannot open level.dat");
    const std::string raw{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
    auto root = nbt::parse_compressed(ByteSpan{
        reinterpret_cast<const std::byte*>(raw.data()), raw.size()});
    if (!root) return std::unexpected{std::move(root.error())};
    auto* data = root->find_mut("Data");
    if (data == nullptr) return make_error(ErrorCode::world, "level.dat missing Data compound");
    data->set("LevelName", nbt::make_string(std::string{new_level_name}));
    auto serialized = nbt::serialize("", *root);
    if (!serialized) return std::unexpected{std::move(serialized.error())};
    auto compressed = proto::deflate_gzip(ByteSpan{*serialized}, proto::kDefaultCompressionLevel);
    if (!compressed) return std::unexpected{std::move(compressed.error())};
    std::ofstream out{path, std::ios::binary | std::ios::trunc};
    if (!out) return make_error(ErrorCode::io, "cannot write level.dat");
    out.write(reinterpret_cast<const char*>(compressed->data()),
              static_cast<std::streamsize>(compressed->size()));
    return out ? Result<void>{} : make_error(ErrorCode::io, "cannot flush level.dat");
}

bool SaveFormatOld::can_load_world(std::string_view save_name) const {
    return std::filesystem::is_directory(saves_directory_ / save_name);
}

std::filesystem::path SaveFormatOld::get_file(std::string_view save_name,
                                              std::string_view file_name) const {
    return saves_directory_ / save_name / file_name;
}

}  // namespace cyane::world::storage
