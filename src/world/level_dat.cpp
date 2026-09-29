#include "cyane/world/level_dat.hpp"

#include <fstream>
#include <istream>

#include "cyane/world/nbt.hpp"

namespace cyane::world {

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
    if (const auto* v = data->find("SpawnX"); v != nullptr) {
        if (const auto s = v->scalar()) {
            info.spawn_x = static_cast<std::int32_t>(*s);
        }
    }
    if (const auto* v = data->find("SpawnY"); v != nullptr) {
        if (const auto s = v->scalar()) {
            info.spawn_y = static_cast<std::int32_t>(*s);
        }
    }
    if (const auto* v = data->find("SpawnZ"); v != nullptr) {
        if (const auto s = v->scalar()) {
            info.spawn_z = static_cast<std::int32_t>(*s);
        }
    }
    return info;
}

} // namespace cyane::world