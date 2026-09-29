#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

#include "cyane/world/blocks.hpp"
#include "cyane/world/blocks.hpp"

namespace cyane::world {

inline constexpr int kChunkSizeX = 16;
inline constexpr int kChunkSizeY = 256;
inline constexpr int kChunkSizeZ = 16;
inline constexpr int kSectionCount = kChunkSizeY / 16;

struct [[nodiscard]] ChunkPos {
    std::int32_t x{0};
    std::int32_t z{0};

    [[nodiscard]] bool operator==(const ChunkPos& other) const noexcept = default;
    [[nodiscard]] auto operator<=>(const ChunkPos&) const noexcept = default;

    [[nodiscard]] static std::int32_t floor_div(std::int32_t value, std::int32_t divisor) noexcept {
        const auto q = value / divisor;
        return (value % divisor != 0 && (value < 0) != (divisor < 0)) ? q - 1 : q;
    }

    [[nodiscard]] static std::optional<ChunkPos> from_world(std::int32_t wx, std::int32_t wz) noexcept {
        // 两轴独立地板除：截断除法会把 x≥0、z<0 象限的 z 偏移一个区块
        return ChunkPos{floor_div(wx, kChunkSizeX), floor_div(wz, kChunkSizeZ)};
    }

    [[nodiscard]] std::int32_t world_x() const noexcept { return x * kChunkSizeX; }
    [[nodiscard]] std::int32_t world_z() const noexcept { return z * kChunkSizeZ; }
};

struct [[nodiscard]] Section {
    std::vector<std::uint16_t> states;     // 4096 个全局方块状态 id
    std::vector<std::uint8_t> block_light; // 2048 字节，4bit/方块
    std::vector<std::uint8_t> sky_light;   // 2048 字节

    [[nodiscard]] bool empty() const noexcept { return states.empty(); }

    [[nodiscard]] std::uint16_t state(std::size_t index) const noexcept {
        return index < states.size() ? states[index] : kStateAir;
    }

    void fill(std::uint16_t state) { states.assign(kSectionBlockCount, state); }

    void set(std::size_t x, std::size_t y, std::size_t z, std::uint16_t value) {
        if (states.size() != kSectionBlockCount) {
            states.assign(kSectionBlockCount, kStateAir);
        }
        states[section_index(x, y, z)] = value;
    }
};

class [[nodiscard]] Chunk {
public:
    Chunk() = default;
    explicit Chunk(ChunkPos pos) : pos_{std::move(pos)} {}

    [[nodiscard]] const ChunkPos& pos() const noexcept { return pos_; }

    [[nodiscard]] Section* section(std::size_t y) noexcept {
        if (y < sections_.size()) {
            return &sections_[y];
        }
        return nullptr;
    }

    [[nodiscard]] const Section* section(std::size_t y) const noexcept {
        if (y < sections_.size()) {
            return &sections_[y];
        }
        return nullptr;
    }

    void set_section(std::size_t y, Section section) {
        if (y >= sections_.size()) {
            sections_.resize(y + 1);
        }
        sections_[y] = std::move(section);
    }

    [[nodiscard]] std::span<const Section> sections() const noexcept { return sections_; }

    [[nodiscard]] Result<std::uint16_t> block_state(std::int32_t wx, std::int32_t wy, std::int32_t wz) const {
        const int sx = wx - pos_.world_x();
        const int sz = wz - pos_.world_z();
        if (sx < 0 || sx >= kChunkSizeX || sz < 0 || sz >= kChunkSizeZ || wy < 0 || wy >= kChunkSizeY) {
            return make_error(ErrorCode::world, "block coordinates out of chunk bounds");
        }
        const std::size_t y = static_cast<std::size_t>(wy) / 16;
        if (y >= sections_.size()) {
            return kStateAir;
        }
        const Section& sec = sections_[y];
        if (sec.empty()) {
            return kStateAir;
        }
        const int local_y = wy % 16;
        return sec.state(section_index(static_cast<std::size_t>(sx), static_cast<std::size_t>(local_y),
                                       static_cast<std::size_t>(sz)));
    }

    void set_block_state(std::int32_t wx, std::int32_t wy, std::int32_t wz, std::uint16_t state) {
        const int sx = wx - pos_.world_x();
        const int sz = wz - pos_.world_z();
        if (sx < 0 || sx >= kChunkSizeX || sz < 0 || sz >= kChunkSizeZ || wy < 0 || wy >= kChunkSizeY) {
            return;
        }
        const std::size_t y = static_cast<std::size_t>(wy) / 16;
        if (y >= sections_.size()) {
            sections_.resize(y + 1);
        }
        sections_[y].set(static_cast<std::size_t>(sx), static_cast<std::size_t>(wy % 16),
                         static_cast<std::size_t>(sz), state);
    }

private:
    ChunkPos pos_;
    std::vector<Section> sections_;
};

}

namespace std {
template <>
struct hash<cyane::world::ChunkPos> {
    std::size_t operator()(const cyane::world::ChunkPos& pos) const noexcept {
        return static_cast<std::size_t>(pos.x) * 31u + static_cast<std::size_t>(pos.z);
    }
};
}
