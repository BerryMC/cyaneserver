#pragma once

#include <cmath>
#include <cstdint>
#include <unordered_set>
#include <vector>
#include "cyane/world/chunk.hpp"
#include "cyane/world/noise.hpp"

namespace cyane::world {

struct ExplosionPos {
    int x{0};
    int y{0};
    int z{0};

    constexpr bool operator==(const ExplosionPos& other) const noexcept {
        return x == other.x && y == other.y && z == other.z;
    }
};

struct ExplosionPosHash {
    std::size_t operator()(const ExplosionPos& pos) const noexcept {
        return (static_cast<std::size_t>(pos.x) * 3129871) ^
               (static_cast<std::size_t>(pos.z) * 116129781) ^
               static_cast<std::size_t>(pos.y);
    }
};

// 1.12.2 原版爆炸动力学模拟（Explosion 逐行转写）
class Explosion {
public:
    Explosion(double x, double y, double z, float size, bool causes_fire = false, bool damages_terrain = true)
        : x_(x), y_(y), z_(z), size_(size), causes_fire_(causes_fire), damages_terrain_(damages_terrain) {}

    // 第一阶段：16x16x16 射线步进抗爆抗性衰减计算
    void do_explosion_a(Chunk& chunk, gen::JavaRandom& rand) {
        if (!damages_terrain_) return;

        affected_blocks_.clear();

        for (int k = 0; k < 16; ++k) {
            for (int i = 0; i < 16; ++i) {
                for (int j = 0; j < 16; ++j) {
                    if (k == 0 || k == 15 || i == 0 || i == 15 || j == 0 || j == 15) {
                        double d0 = static_cast<double>(static_cast<float>(k) / 15.0f * 2.0f - 1.0f);
                        double d1 = static_cast<double>(static_cast<float>(i) / 15.0f * 2.0f - 1.0f);
                        double d2 = static_cast<double>(static_cast<float>(j) / 15.0f * 2.0f - 1.0f);
                        const double d3 = std::sqrt(d0 * d0 + d1 * d1 + d2 * d2);
                        d0 /= d3;
                        d1 /= d3;
                        d2 /= d3;

                        float blast_force = size_ * (0.7f + rand.next_float() * 0.6f);
                        double cur_x = x_;
                        double cur_y = y_;
                        double cur_z = z_;

                        for (float step = 0.3f; blast_force > 0.0f; blast_force -= 0.22500001f) {
                            const int bx = static_cast<int>(std::floor(cur_x));
                            const int by = static_cast<int>(std::floor(cur_y));
                            const int bz = static_cast<int>(std::floor(cur_z));

                            if (bx >= 0 && bx < 16 && bz >= 0 && bz < 16 && by >= 0 && by < 256) {
                                const auto state = chunk.block_at(static_cast<std::size_t>(bx), by, static_cast<std::size_t>(bz));
                                if (state != kStateAir) {
                                    float resistance = 0.5f;
                                    if (state == kStateBedrock) resistance = 3600000.0f;
                                    else if (state == kStateObsidian) resistance = 1200.0f;
                                    else if (state == kStateStone) resistance = 6.0f;

                                    blast_force -= (resistance + 0.3f) * step;
                                }

                                if (blast_force > 0.0f && by >= 0 && by < 256) {
                                    affected_blocks_.insert(ExplosionPos{bx, by, bz});
                                }
                            }

                            cur_x += d0 * static_cast<double>(step);
                            cur_y += d1 * static_cast<double>(step);
                            cur_z += d2 * static_cast<double>(step);
                        }
                    }
                }
            }
        }
    }

    // 第二阶段：方块清除与引燃火焰
    void do_explosion_b(Chunk& chunk, gen::JavaRandom& rand) {
        for (const auto& pos : affected_blocks_) {
            if (pos.x >= 0 && pos.x < 16 && pos.z >= 0 && pos.z < 16 && pos.y >= 0 && pos.y < 256) {
                const auto cur = chunk.block_at(static_cast<std::size_t>(pos.x), pos.y, static_cast<std::size_t>(pos.z));
                if (cur != kStateAir && cur != kStateBedrock) {
                    chunk.set_block(static_cast<std::size_t>(pos.x), pos.y, static_cast<std::size_t>(pos.z), kStateAir);
                }
            }
        }

        if (causes_fire_) {
            for (const auto& pos : affected_blocks_) {
                if (pos.x >= 0 && pos.x < 16 && pos.z >= 0 && pos.z < 16 && pos.y >= 1 && pos.y < 255) {
                    if (chunk.block_at(static_cast<std::size_t>(pos.x), pos.y, static_cast<std::size_t>(pos.z)) == kStateAir &&
                        is_solid(chunk.block_at(static_cast<std::size_t>(pos.x), pos.y - 1, static_cast<std::size_t>(pos.z))) &&
                        rand.next_int(3) == 0) {
                        chunk.set_block(static_cast<std::size_t>(pos.x), pos.y, static_cast<std::size_t>(pos.z), kStateFire);
                    }
                }
            }
        }
    }

    [[nodiscard]] const std::unordered_set<ExplosionPos, ExplosionPosHash>& get_affected_blocks() const noexcept {
        return affected_blocks_;
    }

private:
    double x_;
    double y_;
    double z_;
    float size_;
    bool causes_fire_;
    bool damages_terrain_;
    std::unordered_set<ExplosionPos, ExplosionPosHash> affected_blocks_;
};

}  // namespace cyane::world
