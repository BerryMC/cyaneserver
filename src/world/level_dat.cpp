#include "cyane/world/level_dat.hpp"

#include <fstream>
#include <ctime>
#include <istream>

#include "cyane/proto/frame.hpp"
#include "cyane/world/nbt.hpp"
#include "cyane/world/world_info.hpp"

namespace cyane::world {

using nbt::Compound;

Result<LevelInfo> load_level_dat(const std::filesystem::path& world_dir) {
    LevelInfo info;
    const auto path = world_dir / "level.dat";
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        return info;
    }
    std::ifstream in{path, std::ios::binary};
    if (!in) {
        return make_error(ErrorCode::io, "cannot open level.dat: " + path.string());
    }
    std::string raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    auto root = nbt::parse_compressed(
        ByteSpan{reinterpret_cast<const std::byte*>(raw.data()), raw.size()});
    if (!root) {
        return std::unexpected{std::move(root.error())};
    }
    // level.dat 与 player .dat 不同：字段包在 "Data" compound 下（R-010 / R-011）
    const nbt::Value* data = root->find("Data");
    if (data == nullptr) {
        return make_error(ErrorCode::world, "level.dat missing Data compound");
    }
    // 用 WorldInfo 解析（字段集与写入端一致），再投影到 LevelInfo
    storage::WorldInfo world_info;
    world_info.read_root(*root);
    info.spawn_x = world_info.spawn_x;
    info.spawn_y = world_info.spawn_y;
    info.spawn_z = world_info.spawn_z;
    info.seed = world_info.random_seed;
    return info;
}

Result<storage::WorldInfo> load_world_info(const std::filesystem::path& world_dir) {
    storage::WorldInfo info;
    const auto path = world_dir / "level.dat";
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        return info;
    }
    std::ifstream in{path, std::ios::binary};
    if (!in) {
        return make_error(ErrorCode::io, "cannot open level.dat: " + path.string());
    }
    std::string raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    auto root = nbt::parse_compressed(
        ByteSpan{reinterpret_cast<const std::byte*>(raw.data()), raw.size()});
    if (!root) {
        return std::unexpected{std::move(root.error())};
    }
    info.read_root(*root);
    return info;
}

Result<bool> ensure_level_dat(const std::filesystem::path& world_dir, std::int32_t game_type,
                              std::int64_t seed) {
    const auto path = world_dir / "level.dat";
    std::error_code ec;
    if (std::filesystem::exists(path, ec)) {
        return false;
    }

    // 原版 level.dat 的完整常用字段集（WorldInfo 逐字段转写）。
    // 只写最小集时 Spigot/vanilla 会用默认值补，但 LevelName/Hardcore/Difficulty/
    // GameRules/Weather/LastPlayed 等会丢失，故按原版结构一次写全。
    storage::WorldInfo info;
    info.random_seed = seed;
    info.game_type = static_cast<std::uint8_t>(game_type);
    info.level_name = world_dir.filename().string();
    if (info.level_name.empty()) info.level_name = "world";
    info.spawn_x = 0;
    info.spawn_y = 64;
    info.spawn_z = 0;
    info.initialized = true;
    info.last_played = static_cast<std::int64_t>(std::time(nullptr)) * 1000;
    info.rain_time = 0;
    info.thunder_time = 0;
    const auto root = info.write_to_nbt();
    const auto nbt_bytes = nbt::serialize("", root);
    if (!nbt_bytes) {
        return std::unexpected{std::move(nbt_bytes.error())};
    }
    const auto gz = proto::deflate_gzip(ByteSpan{*nbt_bytes}, proto::kDefaultCompressionLevel);
    if (!gz) {
        return std::unexpected{std::move(gz.error())};
    }

    std::filesystem::create_directories(world_dir, ec);
    if (ec) {
        return make_error(ErrorCode::io, "cannot create world dir: " + ec.message());
    }
    std::ofstream out{path, std::ios::binary | std::ios::trunc};
    if (!out) {
        return make_error(ErrorCode::io, "cannot write level.dat: " + path.string());
    }
    out.write(reinterpret_cast<const char*>(gz->data()),
              static_cast<std::streamsize>(gz->size()));
    out.flush();
    if (!out) {
        return make_error(ErrorCode::io, "cannot flush level.dat: " + path.string());
    }
    return true;
}

} // namespace cyane::world