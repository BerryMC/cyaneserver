#include "cyane/world/level_dat.hpp"

#include <fstream>
#include <istream>

#include "cyane/proto/frame.hpp"
#include "cyane/world/nbt.hpp"

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
    if (const auto* v = data->find("RandomSeed"); v != nullptr) {
        if (const auto s = v->scalar()) {
            info.seed = *s;
        }
    }
    return info;
}

Result<bool> ensure_level_dat(const std::filesystem::path& world_dir, std::int32_t game_type) {
    const auto path = world_dir / "level.dat";
    std::error_code ec;
    if (std::filesystem::exists(path, ec)) {
        return false;
    }

    // 最小集：原版 1.12.2 level.dat 的 Data 字段（缺省值即超平坦世界默认态）
    Compound data;
    data.emplace_back("Time", nbt::Value{nbt::Tag::i64, std::int64_t{0}});
    Compound version;
    version.emplace_back("Id", nbt::Value{nbt::Tag::i32, std::int32_t{1101}});
    version.emplace_back("Name", nbt::Value{nbt::Tag::string, std::string{"1.12.2"}});
    data.emplace_back("Version", nbt::Value{nbt::Tag::compound, std::move(version)});
    data.emplace_back("GameType", nbt::Value{nbt::Tag::i32, game_type});
    data.emplace_back("generatorName", nbt::Value{nbt::Tag::string, std::string{"flat"}});
    data.emplace_back("SpawnX", nbt::Value{nbt::Tag::i32, std::int32_t{0}});
    data.emplace_back("SpawnY", nbt::Value{nbt::Tag::i32, std::int32_t{4}});
    data.emplace_back("SpawnZ", nbt::Value{nbt::Tag::i32, std::int32_t{0}});
    Compound root;
    root.emplace_back("Data", nbt::Value{nbt::Tag::compound, std::move(data)});
    const auto nbt_bytes = nbt::serialize("", nbt::make_compound(std::move(root)));
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