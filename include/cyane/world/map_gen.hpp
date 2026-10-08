#pragma once

#include <cmath>
#include <cstdint>
#include <vector>

#include "cyane/world/blocks.hpp"
#include "cyane/world/chunk.hpp"
#include "cyane/world/noise.hpp"

namespace cyane::world::gen {

// MapGenBase（逐行转写原版 1.12.2 洞穴与峡谷生成基类）
// 原版通过遍历周围 range 个区块，使用确定性种子为每个区块生成洞穴入口。
class MapGenBase {
public:
    int range = 8;

    void generate(std::uint64_t world_seed, int x, int z, Chunk& chunk) {
        JavaRandom rng(world_seed);
        const long long j = static_cast<long long>(rng.next_long());
        const long long k = static_cast<long long>(rng.next_long());

        for (int l = x - range; l <= x + range; ++l) {
            for (int i1 = z - range; i1 <= z + range; ++i1) {
                const long long j1 = static_cast<long long>(l) * j;
                const long long k1 = static_cast<long long>(i1) * k;
                JavaRandom chunk_rng(static_cast<std::uint64_t>(j1 ^ k1 ^ static_cast<long long>(world_seed)));
                recursive_generate(chunk_rng, l, i1, x, z, chunk);
            }
        }
    }

    virtual void recursive_generate(JavaRandom& rng, int chunk_x, int chunk_z,
                                    int origin_x, int origin_z, Chunk& chunk) {
        (void)rng; (void)chunk_x; (void)chunk_z; (void)origin_x; (void)origin_z; (void)chunk;
    }

    virtual ~MapGenBase() = default;
};

// MapGenCaves（逐行转写原版洞穴生成器）
class MapGenCaves : public MapGenBase {
public:
    static constexpr std::uint16_t kLava = 11 << 4;      // 岩浆
    static constexpr std::uint16_t kSandstone = 24 << 4; // 砂岩
    static constexpr std::uint16_t kRedSandstone = 179 << 4;

    void recursive_generate(JavaRandom& rng, int chunk_x, int chunk_z,
                            int origin_x, int origin_z, Chunk& chunk) override {
        int i = rng.next_int(rng.next_int(rng.next_int(15) + 1) + 1);
        if (rng.next_int(7) != 0) {
            i = 0;
        }

        for (int j = 0; j < i; ++j) {
            const double d0 = static_cast<double>(chunk_x * 16 + rng.next_int(16));
            const double d1 = static_cast<double>(rng.next_int(rng.next_int(120) + 8));
            const double d2 = static_cast<double>(chunk_z * 16 + rng.next_int(16));
            int k = 1;

            if (rng.next_int(4) == 0) {
                add_room(rng, origin_x, origin_z, chunk, d0, d1, d2);
                k += rng.next_int(4);
            }

            for (int l = 0; l < k; ++l) {
                const float f = rng.next_float() * 6.2831855f;
                const float f1 = (rng.next_float() - 0.5f) * 2.0f / 8.0f;
                float f2 = rng.next_float() * 2.0f + rng.next_float();
                if (rng.next_int(10) == 0) {
                    f2 *= rng.next_float() * rng.next_float() * 3.0f + 1.0f;
                }
                add_tunnel(static_cast<std::uint64_t>(rng.next_long()), origin_x, origin_z,
                           chunk, d0, d1, d2, f2, f, f1, 0, 0, 1.0);
            }
        }
    }

private:
    void add_room(JavaRandom& rng, int cx, int cz, Chunk& chunk,
                  double x, double y, double z) {
        add_tunnel(static_cast<std::uint64_t>(rng.next_long()), cx, cz, chunk, x, y, z,
                   1.0f + rng.next_float() * 6.0f, 0.0f, 0.0f, -1, -1, 0.5);
    }

    void add_tunnel(std::uint64_t seed, int cx, int cz, Chunk& chunk,
                    double px, double py, double pz,
                    float radius, float yaw, float pitch,
                    int p15, int p16, double height_ratio) {
        const double d0 = static_cast<double>(cx * 16 + 8);
        const double d1 = static_cast<double>(cz * 16 + 8);
        float f = 0.0f;
        float f1 = 0.0f;
        JavaRandom random(seed);

        if (p16 <= 0) {
            const int i = range * 16 - 16;
            p16 = i - random.next_int(i / 4);
        }

        bool flag2 = false;
        if (p15 == -1) {
            p15 = p16 / 2;
            flag2 = true;
        }

        const int j = random.next_int(p16 / 2) + p16 / 4;
        const bool flag = random.next_int(6) == 0;

        for (; p15 < p16; ++p15) {
            const double d2 = 1.5 + std::sin(static_cast<float>(p15) * 3.1415927f / static_cast<float>(p16)) * static_cast<double>(radius);
            const double d3 = d2 * height_ratio;
            const float f2 = std::cos(pitch);
            const float f3 = std::sin(pitch);
            px += static_cast<double>(std::cos(yaw) * f2);
            py += static_cast<double>(f3);
            pz += static_cast<double>(std::sin(yaw) * f2);

            if (flag) {
                pitch *= 0.92f;
            } else {
                pitch *= 0.7f;
            }
            pitch += f1 * 0.1f;
            yaw += f * 0.1f;
            f1 *= 0.9f;
            f *= 0.75f;
            f1 += (random.next_float() - random.next_float()) * random.next_float() * 2.0f;
            f += (random.next_float() - random.next_float()) * random.next_float() * 4.0f;

            if (!flag2 && p15 == j && radius > 1.0f && p16 > 0) {
                add_tunnel(static_cast<std::uint64_t>(random.next_long()), cx, cz, chunk,
                           px, py, pz, random.next_float() * 0.5f + 0.5f,
                           yaw - 1.5707964f, pitch / 3.0f, p15, p16, 1.0);
                add_tunnel(static_cast<std::uint64_t>(random.next_long()), cx, cz, chunk,
                           px, py, pz, random.next_float() * 0.5f + 0.5f,
                           yaw + 1.5707964f, pitch / 3.0f, p15, p16, 1.0);
                return;
            }

            if (flag2 || random.next_int(4) != 0) {
                const double d4 = px - d0;
                const double d5 = pz - d1;
                const double d6 = static_cast<double>(p16 - p15);
                const double d7 = static_cast<double>(radius + 2.0f + 16.0f);

                if (d4 * d4 + d5 * d5 - d6 * d6 > d7 * d7) {
                    return;
                }

                if (px >= d0 - 16.0 - d2 * 2.0 && pz >= d1 - 16.0 - d2 * 2.0 &&
                    px <= d0 + 16.0 + d2 * 2.0 && pz <= d1 + 16.0 + d2 * 2.0) {
                    int k2 = static_cast<int>(std::floor(px - d2)) - cx * 16 - 1;
                    int k = static_cast<int>(std::floor(px + d2)) - cx * 16 + 1;
                    int l2 = static_cast<int>(std::floor(py - d3)) - 1;
                    int l = static_cast<int>(std::floor(py + d3)) + 1;
                    int i3 = static_cast<int>(std::floor(pz - d2)) - cz * 16 - 1;
                    int i1 = static_cast<int>(std::floor(pz + d2)) - cz * 16 + 1;

                    if (k2 < 0) k2 = 0;
                    if (k > 16) k = 16;
                    if (l2 < 1) l2 = 1;
                    if (l > 248) l = 248;
                    if (i3 < 0) i3 = 0;
                    if (i1 > 16) i1 = 16;

                    bool flag3 = false;
                    for (int j1 = k2; !flag3 && j1 < k; ++j1) {
                        for (int k1 = i3; !flag3 && k1 < i1; ++k1) {
                            for (int l1 = l + 1; !flag3 && l1 >= l2 - 1; --l1) {
                                if (l1 >= 0 && l1 < 256) {
                                    const auto blk = chunk.block_at(
                                        static_cast<std::size_t>(j1), l1, static_cast<std::size_t>(k1));
                                    if (block_id(blk) == 8 || block_id(blk) == 9) {
                                        flag3 = true;
                                    }
                                    if (l1 != l2 - 1 && j1 != k2 && j1 != k - 1 && k1 != i3 && k1 != i1 - 1) {
                                        l1 = l2;
                                    }
                                }
                            }
                        }
                    }

                    if (!flag3) {
                        for (int j3 = k2; j3 < k; ++j3) {
                            const double d10 = (static_cast<double>(j3 + cx * 16) + 0.5 - px) / d2;
                            for (int i2 = i3; i2 < i1; ++i2) {
                                const double d8 = (static_cast<double>(i2 + cz * 16) + 0.5 - pz) / d2;
                                if (d10 * d10 + d8 * d8 < 1.0) {
                                    for (int j2 = l; j2 > l2; --j2) {
                                        const double d9 = (static_cast<double>(j2 - 1) + 0.5 - py) / d3;
                                        if (d9 > -0.7 && d10 * d10 + d9 * d9 + d8 * d8 < 1.0) {
                                            dig_block(chunk,
                                                      static_cast<std::size_t>(j3),
                                                      j2,
                                                      static_cast<std::size_t>(i2));
                                        }
                                    }
                                }
                            }
                        }
                        if (flag2) break;
                    }
                }
            }
        }
    }

    [[nodiscard]] static bool can_replace_block(std::uint16_t state) noexcept {
        const auto id = block_id(state);
        switch (id) {
            case 1:  // stone
            case 3:  // dirt
            case 2:  // grass
            case 12: // sand
            case 13: // gravel
            case 24: // sandstone
            case 179: // red sandstone
            case 78: // snow layer
                return true;
            default:
                return false;
        }
    }

    void dig_block(Chunk& chunk, std::size_t x, int y, std::size_t z) {
        const auto state = chunk.block_at(x, y, z);
        if (can_replace_block(state)) {
            if (y - 1 < 10) {
                chunk.set_block(x, y, z, kLava);
            } else {
                chunk.set_block(x, y, z, kStateAir);
            }
        }
    }
};

}  // namespace cyane::world::gen
