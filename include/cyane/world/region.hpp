#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <vector>

#include "cyane/core/bytes.hpp"
#include "cyane/core/error.hpp"

namespace cyane::world {

// Anvil region（r.<rx>.<rz>.mca）：内存中维护整幅文件镜像，支持读区块、
// 写区块（扇位分配/复用）与落盘（tmp + rename 原子替换）。
// 区块载荷 = [长度:3B 大端][版本字节][压缩数据]；1=gzip 2=zlib。
class RegionFile {
public:
    static constexpr int kRegionSize = 32;
    static constexpr std::size_t kSectorSize = 4096;
    static constexpr std::size_t kMaxChunkSectors = 255;

    // 读取现有文件；不存在返回空镜像（写入时按需落盘）
    [[nodiscard]] static Result<RegionFile> load(const std::filesystem::path& path);
    [[nodiscard]] static std::filesystem::path path_for(std::string_view region_dir, std::int32_t rx,
                                                        std::int32_t rz);

    // 区块数据（已解压，含版本字节的原始载荷为压缩形式——这里返回解压后的 NBT 字节）
    [[nodiscard]] Result<std::optional<Bytes>> read_chunk(int cx, int cz) const;
    // 写入并压缩区块；返回 true 表示文件镜像已变化（需要 save）
    [[nodiscard]] Result<bool> write_chunk(int cx, int cz, ByteSpan uncompressed);

    [[nodiscard]] Result<void> save(const std::filesystem::path& path) const;
    [[nodiscard]] bool dirty() const noexcept { return dirty_; }

private:
    [[nodiscard]] std::uint32_t location(int cx, int cz) const noexcept {
        return location_at(static_cast<std::size_t>(cz & (kRegionSize - 1)) * kRegionSize +
                           static_cast<std::size_t>(cx & (kRegionSize - 1)));
    }
    [[nodiscard]] std::uint32_t location_at(std::size_t index) const noexcept {
        const std::size_t offset = index * 4;
        if (offset + 4 > data_.size()) {
            return 0;
        }
        const auto* p = reinterpret_cast<const std::uint8_t*>(data_.data()) + offset;
        return static_cast<std::uint32_t>(p[0] << 16 | p[1] << 8 | p[2]);
    }
    void set_location(int cx, int cz, std::uint32_t sector, std::uint8_t count);
    [[nodiscard]] std::size_t sector_count() const noexcept { return data_.size() / kSectorSize; }
    [[nodiscard]] std::uint8_t sector_bytes(int cx, int cz) const noexcept;
    // 分配 count 个连续扇区；无连续空洞时追加到文件尾
    [[nodiscard]] std::uint32_t allocate(std::size_t count);
    void free(std::uint32_t sector, std::uint8_t count);

    // 重建扇区占用位图（load 后）
    void rebuild_bitmap();

    std::vector<std::byte> data_;
    std::vector<bool> used_;
    bool dirty_{false};
};

} // namespace cyane::world