#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "cyane/core/error.hpp"

namespace cyane::world {

struct [[nodiscard]] RegionFile {
    static constexpr int kRegionSize = 32;
    static constexpr int kSectorSize = 4096;

    std::int32_t rx{0};
    std::int32_t rz{0};
    std::vector<std::uint8_t> data;

    [[nodiscard]] static Result<RegionFile> load(std::string_view path);

    [[nodiscard]] bool has_chunk(std::int32_t cx, std::int32_t cz) const noexcept {
        return offset(cx, cz) != 0;
    }

    [[nodiscard]] std::optional<std::span<const std::uint8_t>> chunk_data(std::int32_t cx, std::int32_t cz) const;

private:
    [[nodiscard]] std::uint32_t offset(std::int32_t cx, std::int32_t cz) const noexcept {
        const std::size_t index = static_cast<std::size_t>(cz % kRegionSize) * kRegionSize +
                                  static_cast<std::size_t>(cx % kRegionSize);
        if (index * 4 + 4 > data.size()) {
            return 0;
        }
        const std::uint8_t* p = data.data() + index * 4;
        return static_cast<std::uint32_t>(p[0] << 16 | p[1] << 8 | p[2]);
    }
};

}
