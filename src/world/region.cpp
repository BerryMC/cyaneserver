#include "cyane/world/region.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>

#include "cyane/core/bytes.hpp"
#include "cyane/core/error.hpp"
#include "cyane/proto/frame.hpp"

namespace cyane::world {

Result<RegionFile> RegionFile::load(std::string_view path) {
    RegionFile rf;
    std::ifstream file{std::string(path), std::ios::binary};
    if (!file) {
        return make_error(ErrorCode::io, "region file not found: " + std::string(path));
    }
    file.seekg(0, std::ios::end);
    const auto size = file.tellg();
    if (size < 2) {
        return make_error(ErrorCode::io, "region file too small");
    }
    file.seekg(0, std::ios::beg);
    rf.data.resize(static_cast<std::size_t>(size));
    file.read(reinterpret_cast<char*>(rf.data.data()), size);
    return rf;
}

std::optional<std::span<const std::uint8_t>> RegionFile::chunk_data(std::int32_t cx, std::int32_t cz) const {
    const auto off = offset(cx, cz);
    if (off == 0) {
        return std::nullopt;
    }
    const std::size_t sector = off * kSectorSize;
    if (sector + kSectorSize > data.size()) {
        return std::nullopt;
    }
    const std::uint8_t* hdr = data.data() + sector;
    const std::uint32_t chunk_size = (static_cast<std::uint32_t>(hdr[0]) << 16) |
                                        (static_cast<std::uint32_t>(hdr[1]) << 8) |
                                         static_cast<std::uint32_t>(hdr[2]);
    if (chunk_size == 0 || sector + kSectorSize + chunk_size > data.size()) {
        return std::nullopt;
    }
    const std::uint8_t* chunk_ptr = data.data() + sector + kSectorSize;
    // Check if this is compressed (version byte is 2 or greater)
    if (chunk_ptr[0] >= 2) {
        // This is a compressed NBT chunk, decompress it
        auto inflated = cyane::proto::inflate(
            ByteSpan{reinterpret_cast<const std::byte*>(chunk_ptr), chunk_size - 1}, 0);
        if (!inflated) {
            // Decompression failed, return the raw compressed data as fallback
            return std::span<const std::uint8_t>(chunk_ptr + 1, chunk_size - 1);
        }
        // Return the decompressed data as uint8_t span
        const auto* p = reinterpret_cast<const std::uint8_t*>(inflated->data());
        return std::span<const std::uint8_t>(p, inflated->size());
    }
    // This is an uncompressed chunk (version byte is 1)
    return std::span<const std::uint8_t>(chunk_ptr + 1, chunk_size - 1);
}

}
