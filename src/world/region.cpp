#include "cyane/world/region.hpp"

#include <array>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <format>
#include <fstream>

#include "cyane/proto/frame.hpp"

namespace cyane::world {

namespace {

constexpr std::size_t kSectorBytes = RegionFile::kSectorSize;
constexpr std::size_t kHeaderBytes = 2 * kSectorBytes;  // 位置表 + 时间戳表
constexpr std::uint8_t kVersionZlib = 2;

// 4 字节大端：区块记录长度字段
[[nodiscard]] std::uint32_t read_u32(const std::byte* p) noexcept {
    const auto* u = reinterpret_cast<const std::uint8_t*>(p);
    return static_cast<std::uint32_t>(static_cast<std::uint32_t>(u[0]) << 24 |
                                      static_cast<std::uint32_t>(u[1]) << 16 |
                                      static_cast<std::uint32_t>(u[2]) << 8 |
                                      static_cast<std::uint32_t>(u[3]));
}

void write_u32(std::byte* p, std::uint32_t value) noexcept {
    auto* u = reinterpret_cast<std::uint8_t*>(p);
    u[0] = static_cast<std::uint8_t>((value >> 24) & 0xFF);
    u[1] = static_cast<std::uint8_t>((value >> 16) & 0xFF);
    u[2] = static_cast<std::uint8_t>((value >> 8) & 0xFF);
    u[3] = static_cast<std::uint8_t>(value & 0xFF);
}

void write_u24(std::byte* p, std::uint32_t value) noexcept {
    auto* u = reinterpret_cast<std::uint8_t*>(p);
    u[0] = static_cast<std::uint8_t>((value >> 16) & 0xFF);
    u[1] = static_cast<std::uint8_t>((value >> 8) & 0xFF);
    u[2] = static_cast<std::uint8_t>(value & 0xFF);
}

} // namespace

Result<RegionFile> RegionFile::load(const std::filesystem::path& path) {
    RegionFile rf;
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        return rf;  // 空镜像：全新 region
    }
    std::ifstream in{path, std::ios::binary};
    if (!in) {
        return make_error(ErrorCode::io, "cannot open region file: " + path.string());
    }
    in.seekg(0, std::ios::end);
    const auto size = static_cast<std::size_t>(in.tellg());
    in.seekg(0, std::ios::beg);
    // 最小 8KiB 头；被截断的文件按缺失处理，避免崩溃
    if (size < kHeaderBytes) {
        return rf;
    }
    rf.data_.resize(size);
    in.read(reinterpret_cast<char*>(rf.data_.data()), static_cast<std::streamsize>(size));
    if (!in) {
        return make_error(ErrorCode::io, "cannot read region file: " + path.string());
    }
    rf.rebuild_bitmap();
    return rf;
}

std::filesystem::path RegionFile::path_for(std::string_view region_dir, std::int32_t rx,
                                           std::int32_t rz) {
    return std::filesystem::path{region_dir} /
           std::format("r.{}.{}.mca", rx, rz);
}

Result<std::optional<Bytes>> RegionFile::read_chunk(int cx, int cz) const {
    const std::uint32_t loc = location(cx, cz);
    if (loc == 0) {
        return std::nullopt;
    }
    const std::size_t sector = static_cast<std::size_t>(loc);
    const std::uint8_t count = sector_bytes(cx, cz);
    const std::size_t byte_offset = sector * kSectorBytes;
    if (byte_offset + static_cast<std::size_t>(count) * kSectorBytes > data_.size()) {
        return make_error(ErrorCode::world, "region chunk location out of range");
    }
    // 区块记录：4 字节大端长度（含压缩字节）+ 1 字节压缩类型 + 压缩数据
    const std::uint32_t length = read_u32(data_.data() + byte_offset);
    if (length < 1) {
        return std::nullopt;  // 空块
    }
    const std::size_t available = static_cast<std::size_t>(count) * kSectorBytes - 5;
    if (length - 1 > available) {
        return make_error(ErrorCode::world, "region chunk length exceeds its sectors");
    }
    const auto compression = std::to_integer<std::uint8_t>(data_[byte_offset + 4]);
    const auto payload = ByteSpan{data_.data() + byte_offset + 5, length - 1};
    if (compression == 3) {
        return std::optional<Bytes>{Bytes{payload.begin(), payload.end()}};
    }
    auto inflated = proto::inflate_dynamic(payload, 8u << 20, compression == 1);
    if (!inflated) {
        return std::unexpected{std::move(inflated.error())};
    }
    return std::optional<Bytes>{std::move(*inflated)};
}

Result<bool> RegionFile::write_chunk(int cx, int cz, ByteSpan uncompressed) {
    auto compressed = proto::deflate(uncompressed, proto::kDefaultCompressionLevel);
    if (!compressed) {
        return std::unexpected{std::move(compressed.error())};
    }
    // 记录 = 4 字节长度（含压缩字节）+ 压缩类型字节 + 压缩数据
    const std::size_t payload_size = 1 + compressed->size();
    const std::size_t entry_size = 4 + payload_size;
    if (entry_size > RegionFile::kMaxChunkSectors * kSectorBytes) {
        return make_error(ErrorCode::world, "region chunk too large");
    }
    const std::size_t needed = (entry_size + kSectorBytes - 1) / kSectorBytes;

    const std::uint32_t old_loc = location(cx, cz);
    const std::uint8_t old_count = old_loc != 0 ? sector_bytes(cx, cz) : 0;
    if (old_loc != 0 && static_cast<std::size_t>(old_count) == needed) {
        // 原地覆写
        std::byte* base = data_.data() + static_cast<std::size_t>(old_loc) * kSectorBytes;
        write_u32(base, static_cast<std::uint32_t>(payload_size));
        base[4] = std::byte{kVersionZlib};
        std::memcpy(base + 5, compressed->data(), compressed->size());
        dirty_ = true;
        return true;
    }
    if (old_loc != 0) {
        free(old_loc, old_count);
    }
    const std::uint32_t sector = allocate(needed);
    if (static_cast<std::size_t>(sector) * kSectorBytes + needed * kSectorBytes > data_.size()) {
        data_.resize((static_cast<std::size_t>(sector) + needed) * kSectorBytes);
    }
    std::byte* base = data_.data() + static_cast<std::size_t>(sector) * kSectorBytes;
    write_u32(base, static_cast<std::uint32_t>(payload_size));
    base[4] = std::byte{kVersionZlib};
    std::memcpy(base + 5, compressed->data(), compressed->size());
    set_location(cx, cz, sector, static_cast<std::uint8_t>(needed));

    // 时间戳表：位置表后 1024 字节，秒级 unix 时间
    const auto now = static_cast<std::uint32_t>(::time(nullptr));
    const std::size_t ts_index = (static_cast<std::size_t>(cz & (kRegionSize - 1)) * kRegionSize +
                                  static_cast<std::size_t>(cx & (kRegionSize - 1))) * 4;
    std::byte* ts = data_.data() + kSectorBytes + ts_index;
    ts[0] = std::byte{static_cast<std::uint8_t>(now >> 24)};
    ts[1] = std::byte{static_cast<std::uint8_t>(now >> 16)};
    ts[2] = std::byte{static_cast<std::uint8_t>(now >> 8)};
    ts[3] = std::byte{static_cast<std::uint8_t>(now)};
    dirty_ = true;
    return true;
}

Result<void> RegionFile::save(const std::filesystem::path& path) const {
    std::filesystem::create_directories(path.parent_path());
    const auto tmp = path.string() + ".tmp";
    {
        std::ofstream out{tmp, std::ios::binary | std::ios::trunc};
        if (!out) {
            return make_error(ErrorCode::io, "cannot create region tmp file: " + tmp);
        }
        out.write(reinterpret_cast<const char*>(data_.data()),
                  static_cast<std::streamsize>(data_.size()));
        if (!out) {
            return make_error(ErrorCode::io, "cannot write region tmp file: " + tmp);
        }
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        return make_error(ErrorCode::io, "cannot replace region file: " + ec.message());
    }
    return {};
}

std::uint8_t RegionFile::sector_bytes(int cx, int cz) const noexcept {
    const std::size_t index = static_cast<std::size_t>(cz & (kRegionSize - 1)) * kRegionSize +
                              static_cast<std::size_t>(cx & (kRegionSize - 1));
    const std::size_t offset = index * 4;
    if (offset + 4 > data_.size()) {
        return 0;
    }
    const auto* p = reinterpret_cast<const std::uint8_t*>(data_.data()) + offset;
    return p[3];
}

void RegionFile::set_location(int cx, int cz, std::uint32_t sector, std::uint8_t count) {
    const std::size_t index = static_cast<std::size_t>(cz & (kRegionSize - 1)) * kRegionSize +
                              static_cast<std::size_t>(cx & (kRegionSize - 1));
    std::byte* p = data_.data() + index * 4;
    write_u24(p, sector);
    p[3] = std::byte{count};
}

std::uint32_t RegionFile::allocate(std::size_t count) {
    // 确保头部两扇区存在（全新 region 的 data_ 初始为空）
    if (data_.size() < kHeaderBytes) {
        data_.resize(kHeaderBytes);
    }
    if (used_.size() < 2) {
        used_.resize(2, false);
        used_[0] = used_[1] = true;
    }
    // 首个连续空闲段；扫到文件尾仍不够则紧接末尾追加
    std::size_t run = 0;
    const std::size_t total = sector_count();
    for (std::size_t s = 2; s <= total; ++s) {
        const bool occupied = s < total && s < used_.size() && used_[s];
        if (!occupied) {
            if (++run == count) {
                const std::uint32_t sector = static_cast<std::uint32_t>(s - count + 1);
                for (std::size_t i = 0; i < count; ++i) {
                    if (sector + i >= used_.size()) {
                        used_.resize(sector + i + 1, false);
                    }
                    used_[sector + i] = true;
                }
                return sector;
            }
        } else {
            run = 0;
        }
    }
    const std::uint32_t sector = static_cast<std::uint32_t>(total);
    for (std::size_t i = 0; i < count; ++i) {
        if (sector + i >= used_.size()) {
            used_.resize(sector + i + 1, false);
        }
        used_[sector + i] = true;
    }
    return sector;
}

void RegionFile::free(std::uint32_t sector, std::uint8_t count) {
    for (std::uint8_t i = 0; i < count; ++i) {
        if (sector + i < used_.size()) {
            used_[sector + i] = false;
        }
    }
}

void RegionFile::rebuild_bitmap() {
    const std::size_t total = sector_count();
    used_.assign(total, false);
    used_[0] = used_[1] = true;  // 头部
    for (std::size_t index = 0; index < kRegionSize * kRegionSize; ++index) {
        const std::uint32_t loc = location_at(index);
        const auto* p = reinterpret_cast<const std::uint8_t*>(data_.data()) + index * 4;
        const auto count = p[3];
        if (loc == 0 || count == 0) {
            continue;
        }
        for (std::uint8_t i = 0; i < count && loc + i < total; ++i) {
            used_[loc + i] = true;
        }
    }
}

} // namespace cyane::world