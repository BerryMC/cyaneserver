#include "cyane/world/save_handler.hpp"

#include <array>
#include <bit>
#include <chrono>
#include <fstream>
#include <iterator>
#include <string>

#include "cyane/proto/frame.hpp"
#include "cyane/world/nbt.hpp"

namespace cyane::world::storage {

Result<void> SaveHandler::open(const std::filesystem::path& world_dir,
                               std::optional<std::int64_t> initialization_time_ms,
                               bool store_player_data) {
    if (initialized_) {
        return make_error(ErrorCode::world, "save handler already opened");
    }
    world_dir_ = world_dir;
    initialization_time_ = initialization_time_ms.value_or(now_ms());
    std::error_code ec;
    std::filesystem::create_directories(world_dir_, ec);
    if (ec) {
        return make_error(ErrorCode::io, "Failed to check session lock, aborting");
    }
    // Java mkdirs 的结果不影响 session.lock 的创建。
    if (store_player_data) std::filesystem::create_directories(player_dir(), ec);
    std::filesystem::create_directories(data_dir(), ec);

    std::array<unsigned char, 8> bytes{};
    const auto bits = std::bit_cast<std::uint64_t>(initialization_time_);
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        bytes[i] = static_cast<unsigned char>(bits >> (56 - 8 * i));
    }
    std::ofstream out{session_lock_path(), std::ios::binary | std::ios::trunc};
    if (!out) {
        return make_error(ErrorCode::io, "Failed to check session lock, aborting");
    }
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    out.close();
    if (!out) {
        return make_error(ErrorCode::io, "Failed to check session lock, aborting");
    }
    initialized_ = true;
    return {};
}

Result<void> SaveHandler::check_session_lock() const {
    if (!initialized_) {
        return make_error(ErrorCode::world, "save handler not opened");
    }
    std::array<unsigned char, 8> bytes{};
    std::ifstream in{session_lock_path(), std::ios::binary};
    in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!in) {
        return make_error(ErrorCode::world, "Failed to check session lock, aborting");
    }
    std::uint64_t bits = 0;
    for (const auto byte : bytes) {
        bits = (bits << 8) | byte;
    }
    if (std::bit_cast<std::int64_t>(bits) != initialization_time_) {
        return make_error(ErrorCode::world, "The save is being accessed from another location, aborting");
    }
    return {};
}

Result<WorldInfo> SaveHandler::load_world_info() const {
    auto read = [](const std::filesystem::path& path) -> Result<WorldInfo> {
        std::ifstream in{path, std::ios::binary};
        if (!in) return make_error(ErrorCode::io, "cannot open level data: " + path.string());
        const std::string raw{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
        auto root = nbt::parse_compressed(ByteSpan{
            reinterpret_cast<const std::byte*>(raw.data()), raw.size()});
        if (!root) return std::unexpected{std::move(root.error())};
        WorldInfo info;
        info.read_root(*root);
        return info;
    };

    if (auto info = read(level_dat()); info) return info;
    if (std::filesystem::exists(level_dat_old())) return read(level_dat_old());
    return make_error(ErrorCode::io, "cannot read level.dat or level.dat_old");
}

Result<void> SaveHandler::save_world_info(const WorldInfo& info) const {
    if (auto checked = check_session_lock(); !checked) {
        return std::unexpected{std::move(checked.error())};
    }
    // 原版 AnvilSaveHandler.saveWorldInfoWithPlayer 每次保存前强制 setSaveVersion(19133)
    WorldInfo patched = info;
    patched.save_version = 19133;
    auto serialized = nbt::serialize("", patched.write_to_nbt());
    if (!serialized) return std::unexpected{std::move(serialized.error())};
    auto compressed = proto::deflate_gzip(ByteSpan{*serialized}, proto::kDefaultCompressionLevel);
    if (!compressed) return std::unexpected{std::move(compressed.error())};

    const auto next = world_dir_ / "level.dat_new";
    {
        std::ofstream out{next, std::ios::binary | std::ios::trunc};
        if (!out) return make_error(ErrorCode::io, "cannot write level.dat_new");
        out.write(reinterpret_cast<const char*>(compressed->data()),
                  static_cast<std::streamsize>(compressed->size()));
        out.close();
        if (!out) return make_error(ErrorCode::io, "cannot flush level.dat_new");
    }

    std::error_code ec;
    // 对齐 SaveHandler.saveWorldInfoWithPlayer 的顺序：删旧备份 → 当前改名为 old →
    // 如当前仍存在则删除 → new 改名为当前 → 如 new 仍存在则删除。
    std::filesystem::remove(level_dat_old(), ec);
    ec.clear();
    if (std::filesystem::exists(level_dat(), ec)) {
        std::filesystem::rename(level_dat(), level_dat_old(), ec);
        if (ec) return make_error(ErrorCode::io, "cannot rotate level.dat to level.dat_old: " + ec.message());
    }
    ec.clear();
    std::filesystem::remove(level_dat(), ec);
    ec.clear();
    std::filesystem::rename(next, level_dat(), ec);
    if (ec) return make_error(ErrorCode::io, "cannot promote level.dat_new: " + ec.message());
    ec.clear();
    std::filesystem::remove(next, ec);
    return {};
}

std::filesystem::path SaveHandler::map_file(std::string_view name) const {
    return data_dir() / (std::string{name} + ".dat");
}

std::int64_t SaveHandler::now_ms() noexcept {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

}  // namespace cyane::world::storage
