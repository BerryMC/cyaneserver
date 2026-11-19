#pragma once

#include <cstdint>

#include "cyane/world/noise.hpp"

namespace cyane::world::structure {

// World.setRandomSeed：结构放置的种子派生（原版 world/World.java:4079）。
//   long j2 = (long)seedX * 341873128712L + (long)seedY * 132897987541L
//             + worldSeed + (long)seedZ;
//   rand.setSeed(j2);
// 加法与乘法都是 64 位回绕，C++ 侧用无符号承载避免有符号溢出 UB。
inline void set_structure_seed(gen::JavaRandom& rand, std::int64_t world_seed,
                               std::int32_t seed_x, std::int32_t seed_y,
                               std::int32_t seed_z) noexcept {
    const auto x = static_cast<std::uint64_t>(static_cast<std::int64_t>(seed_x)) *
                   static_cast<std::uint64_t>(341873128712LL);
    const auto y = static_cast<std::uint64_t>(static_cast<std::int64_t>(seed_y)) *
                   static_cast<std::uint64_t>(132897987541LL);
    const auto z = static_cast<std::uint64_t>(static_cast<std::int64_t>(seed_z));
    const auto world = static_cast<std::uint64_t>(world_seed);
    rand = gen::JavaRandom{x + y + world + z};
}


// MapGenScatteredFeature.canSpawnStructureAtCoords 的通用形式：
// 每个 max_distance × max_distance 的"网格单元"里恰好有一个起始区块，
// 偏移量由 nextInt(max_distance - 8) 决定。负坐标先做 -(max_distance-1) 修正，
// 再按 Java 的向零截断做整除。
[[nodiscard]] inline bool is_scattered_start(std::int64_t world_seed, std::int32_t chunk_x,
                                             std::int32_t chunk_z, std::int32_t max_distance,
                                             std::int32_t salt) noexcept {
    const std::int32_t original_x = chunk_x;
    const std::int32_t original_z = chunk_z;
    if (chunk_x < 0) {
        chunk_x -= max_distance - 1;
    }
    if (chunk_z < 0) {
        chunk_z -= max_distance - 1;
    }
    std::int32_t k = chunk_x / max_distance;
    std::int32_t l = chunk_z / max_distance;

    gen::JavaRandom rand{0};
    set_structure_seed(rand, world_seed, k, l, salt);

    k *= max_distance;
    l *= max_distance;
    k += rand.next_int(max_distance - 8);
    l += rand.next_int(max_distance - 8);
    return original_x == k && original_z == l;
}

}  // namespace cyane::world::structure
