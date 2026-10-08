#pragma once

#include <cstdint>
#include <vector>

namespace cyane::world::gen {

// Java Random（用于噪声种子初始化，逐行转写 java.util.Random）
// 原版 NoiseGeneratorImproved / NoiseGeneratorSimplex 构造时用 Random(seed) 洗牌
// 排列表。此处只复现 nextDouble / nextInt(bound) / nextLong 三个方法。
class JavaRandom {
public:
    explicit JavaRandom(std::uint64_t seed) {
        seed_ = (seed ^ 0x5DEECE66DULL) & ((1ULL << 48) - 1);
    }

    [[nodiscard]] int next_int(int bound) {
        if ((bound & (bound - 1)) == 0) {
            return static_cast<int>((static_cast<std::uint64_t>(next(31)) * static_cast<std::uint64_t>(bound)) >> 31);
        }
        int bits;
        do {
            bits = next(31);
        } while (bits - (bits % bound) + (bound - 1) < 0);
        return bits % bound;
    }

    [[nodiscard]] double next_double() {
        const auto l = (static_cast<std::int64_t>(next(26)) << 27) + static_cast<std::int64_t>(next(27));
        return static_cast<double>(l) * 1.1102230246251565e-16;
    }

    // nextFloat() = next(24) / (2^24)（java.util.Random.nextFloat）
    [[nodiscard]] float next_float() {
        return static_cast<float>(static_cast<std::uint32_t>(next(24))) / static_cast<float>(1U << 24);
    }

    [[nodiscard]] std::uint64_t next_long() {
        return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(next(32))) << 32) |
               static_cast<std::uint64_t>(static_cast<std::uint32_t>(next(32)));
    }

private:
    [[nodiscard]] int next(int bits) {
        seed_ = (seed_ * 0x5DEECE66DULL + 0xBULL) & ((1ULL << 48) - 1);
        return static_cast<int>(seed_ >> (48 - bits));
    }

    std::uint64_t seed_{0};
};

// NoiseGeneratorImproved（Perlin 改进噪声，逐行转写）
// 用于 NoiseGeneratorOctaves 的八度叠加源噪声。
class NoiseGeneratorImproved {
public:
    static constexpr double kGradX[] = {1,-1,1,-1,1,-1,1,-1,0,0,0,0,1,0,-1,0};
    static constexpr double kGradY[] = {1,1,-1,-1,0,0,0,0,1,-1,1,-1,1,-1,1,-1};
    static constexpr double kGradZ[] = {0,0,0,0,1,1,-1,-1,1,1,-1,-1,0,1,0,-1};
    static constexpr double kGrad2X[] = {1,-1,1,-1,1,-1,1,-1,0,0,0,0,1,0,-1,0};
    static constexpr double kGrad2Z[] = {0,0,0,0,1,1,-1,-1,1,1,-1,-1,0,1,0,-1};

    explicit NoiseGeneratorImproved(JavaRandom& rng)
        : x_coord_(rng.next_double() * 256.0)
        , y_coord_(rng.next_double() * 256.0)
        , z_coord_(rng.next_double() * 256.0) {
        for (int i = 0; i < 256; ++i) {
            permutations_[i] = i;
        }
        for (int l = 0; l < 256; ++l) {
            int j = rng.next_int(256 - l) + l;
            int k = permutations_[l];
            permutations_[l] = permutations_[j];
            permutations_[j] = k;
            permutations_[l + 256] = permutations_[l];
        }
    }

    [[nodiscard]] static double lerp(double t, double a, double b) noexcept {
        return a + t * (b - a);
    }

    [[nodiscard]] static double grad2(int hash, double x, double z) noexcept {
        int i = hash & 15;
        return kGrad2X[i] * x + kGrad2Z[i] * z;
    }

    [[nodiscard]] static double grad(int hash, double x, double y, double z) noexcept {
        int i = hash & 15;
        return kGradX[i] * x + kGradY[i] * y + kGradZ[i] * z;
    }

    // 逐行转写 populateNoiseArray
    void populate_noise_array(std::vector<double>& noise, double x_off, double y_off, double z_off,
                              int x_size, int y_size, int z_size,
                              double x_scale, double y_scale, double z_scale, double noise_scale) const {
        if (y_size == 1) {
            // 原版 ySize==1 快速路径（2D Perlin，用于 surface noise 等）
            // 逐行转写原版 ySize==1 分支
            int i5 = 0, j5 = 0, j = 0, k5 = 0;
            std::size_t l5 = 0;
            double d14 = 0, d15 = 0;
            double d16 = 1.0 / noise_scale;
            for (int j2 = 0; j2 < x_size; ++j2) {
                double d17 = x_off + j2 * x_scale + x_coord_;
                int i6 = static_cast<int>(d17);
                if (d17 < i6) --i6;
                int k2 = i6 & 255;
                d17 -= i6;
                double d18 = d17 * d17 * d17 * (d17 * (d17 * 6.0 - 15.0) + 10.0);
                for (int j6 = 0; j6 < z_size; ++j6) {
                    double d19 = z_off + j6 * z_scale + z_coord_;
                    int k6 = static_cast<int>(d19);
                    if (d19 < k6) --k6;
                    int l6 = k6 & 255;
                    d19 -= k6;
                    double d20 = d19 * d19 * d19 * (d19 * (d19 * 6.0 - 15.0) + 10.0);
                    i5 = permutations_[k2] + 0;
                    j5 = permutations_[i5] + l6;
                    j = permutations_[k2 + 1] + 0;
                    k5 = permutations_[j] + l6;
                    d14 = lerp(d18, grad2(permutations_[j5], d17, d19), grad(permutations_[k5], d17 - 1, 0, d19));
                    d15 = lerp(d18, grad(permutations_[j5 + 1], d17, 0, d19 - 1), grad(permutations_[k5 + 1], d17 - 1, 0, d19 - 1));
                    double d21 = lerp(d20, d14, d15);
                    noise[l5++] += d21 * d16;
                }
            }
        } else {
            // 原版 3D Perlin 分支
            std::size_t i = 0;
            double d0 = 1.0 / noise_scale;
            int k = -1, l = 0, i1 = 0, j1 = 0, k1 = 0, l1 = 0, i2 = 0;
            double d1 = 0, d2 = 0, d3 = 0, d4 = 0;
            for (int l2 = 0; l2 < x_size; ++l2) {
                double d5 = x_off + l2 * x_scale + x_coord_;
                int i3 = static_cast<int>(d5);
                if (d5 < i3) --i3;
                int j3 = i3 & 255;
                d5 -= i3;
                double d6 = d5 * d5 * d5 * (d5 * (d5 * 6.0 - 15.0) + 10.0);
                for (int k3 = 0; k3 < z_size; ++k3) {
                    double d7 = z_off + k3 * z_scale + z_coord_;
                    int l3 = static_cast<int>(d7);
                    if (d7 < l3) --l3;
                    int i4 = l3 & 255;
                    d7 -= l3;
                    double d8 = d7 * d7 * d7 * (d7 * (d7 * 6.0 - 15.0) + 10.0);
                    for (int j4 = 0; j4 < y_size; ++j4) {
                        double d9 = y_off + j4 * y_scale + y_coord_;
                        int k4 = static_cast<int>(d9);
                        if (d9 < k4) --k4;
                        int l4 = k4 & 255;
                        d9 -= k4;
                        double d10 = d9 * d9 * d9 * (d9 * (d9 * 6.0 - 15.0) + 10.0);
                        if (j4 == 0 || l4 != k) {
                            k = l4;
                            l = permutations_[j3] + l4;
                            i1 = permutations_[l] + i4;
                            j1 = permutations_[l + 1] + i4;
                            k1 = permutations_[j3 + 1] + l4;
                            l1 = permutations_[k1] + i4;
                            i2 = permutations_[k1 + 1] + i4;
                            d1 = lerp(d6, grad(permutations_[i1], d5, d9, d7), grad(permutations_[l1], d5 - 1, d9, d7));
                            d2 = lerp(d6, grad(permutations_[j1], d5, d9 - 1, d7), grad(permutations_[i2], d5 - 1, d9 - 1, d7));
                            d3 = lerp(d6, grad(permutations_[i1 + 1], d5, d9, d7 - 1), grad(permutations_[l1 + 1], d5 - 1, d9, d7 - 1));
                            d4 = lerp(d6, grad(permutations_[j1 + 1], d5, d9 - 1, d7 - 1), grad(permutations_[i2 + 1], d5 - 1, d9 - 1, d7 - 1));
                        }
                        double d11 = lerp(d10, d1, d2);
                        double d12 = lerp(d10, d3, d4);
                        double d13 = lerp(d8, d11, d12);
                        noise[i++] += d13 * d0;
                    }
                }
            }
        }
    }

private:
    int permutations_[512]{};
    double x_coord_{0};
    double y_coord_{0};
    double z_coord_{0};

public:
    // 简化版双线性插值噪声（用于生物群系与地表噪声的快速查询）
    // 使用 getValue(x, z) 的 2D Perlin，对结果做单点采样
    [[nodiscard]] double populate_noise_array_bilinear(double x, double z) const {
        // 使用 grad2 的单点 Perlin 采样（近似原版 NoiseGeneratorPerlin.getValue）
        x += x_coord_;
        z += z_coord_;
        int xi = static_cast<int>(x);
        int zi = static_cast<int>(z);
        if (x < xi) --xi;
        if (z < zi) --zi;
        xi &= 255;
        zi &= 255;
        double xf = x - static_cast<int>(x);
        double zf = z - static_cast<int>(z);
        // smoothstep
        double u = xf * xf * xf * (xf * (xf * 6.0 - 15.0) + 10.0);
        double v = zf * zf * zf * (zf * (zf * 6.0 - 15.0) + 10.0);
        int aa = permutations_[permutations_[xi] + zi];
        int ab = permutations_[permutations_[xi] + zi + 1];
        int ba = permutations_[permutations_[xi + 1] + zi];
        int bb = permutations_[permutations_[xi + 1] + zi + 1];
        double x1 = lerp(u, grad2(aa, xf, zf), grad2(ba, xf - 1, zf));
        double x2 = lerp(u, grad2(ab, xf, zf - 1), grad2(bb, xf - 1, zf - 1));
        return lerp(v, x1, x2);
    }
};

// NoiseGeneratorOctaves（八度叠加，逐行转写）
class NoiseGeneratorOctaves {
public:
    NoiseGeneratorOctaves(JavaRandom& rng, int octaves) : octaves_(octaves) {
        generators_.reserve(static_cast<std::size_t>(octaves));
        for (int i = 0; i < octaves; ++i) {
            generators_.emplace_back(rng);
        }
    }

    [[nodiscard]] std::vector<double> generate_noise_octaves(
        std::vector<double>& noise, int x_off, int y_off, int z_off,
        int x_size, int y_size, int z_size,
        double x_scale, double y_scale, double z_scale) {
        const auto total_size = static_cast<std::size_t>(x_size * y_size * z_size);
        if (noise.empty() || noise.size() < total_size) {
            noise.assign(total_size, 0.0);
        } else {
            std::fill(noise.begin(), noise.end(), 0.0);
        }
        double d3 = 1.0;
        for (std::size_t j = 0; j < static_cast<std::size_t>(octaves_); ++j) {
            double d0 = x_off * d3 * x_scale;
            double d1 = y_off * d3 * y_scale;
            double d2 = z_off * d3 * z_scale;
            long long k = static_cast<long long>(d0);
            long long l = static_cast<long long>(d2);
            d0 -= static_cast<double>(k);
            d2 -= static_cast<double>(l);
            k %= 16777216LL;
            l %= 16777216LL;
            d0 += static_cast<double>(k);
            d2 += static_cast<double>(l);
            generators_[j].populate_noise_array(noise, d0, d1, d2, x_size, y_size, z_size,
                                                 x_scale * d3, y_scale * d3, z_scale * d3, d3);
            d3 /= 2.0;
        }
        return noise;
    }

    // 2D 版本（原版 generateNoiseOctaves 的 8 参数重载，用于 depth noise）
    [[nodiscard]] std::vector<double> generate_noise_octaves_2d(
        std::vector<double>& noise, double x_off, double z_off,
        int x_size, int z_size,
        double x_scale, double z_scale, double exponent) {
        const auto total_size = static_cast<std::size_t>(x_size * z_size);
        if (noise.empty() || noise.size() < total_size) {
            noise.assign(total_size, 0.0);
        } else {
            std::fill(noise.begin(), noise.end(), 0.0);
        }
        double d3 = 1.0;
        for (std::size_t j = 0; j < static_cast<std::size_t>(octaves_); ++j) {
            double d0 = x_off * d3 * x_scale;
            double d2 = z_off * d3 * z_scale;
            long long k = static_cast<long long>(d0);
            long long l = static_cast<long long>(d2);
            d0 -= static_cast<double>(k);
            d2 -= static_cast<double>(l);
            k %= 16777216LL;
            l %= 16777216LL;
            d0 += static_cast<double>(k);
            d2 += static_cast<double>(l);
            generators_[j].populate_noise_array(noise, d0, 0, d2, x_size, 1, z_size,
                                                 x_scale * d3, 1.0, z_scale * d3, d3);
            d3 *= exponent;
        }
        return noise;
    }

private:
    int octaves_{0};
    std::vector<NoiseGeneratorImproved> generators_;
};

}  // namespace cyane::world::gen
