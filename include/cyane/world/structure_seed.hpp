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

}  // namespace cyane::world::structure
