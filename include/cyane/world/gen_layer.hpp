#pragma once

#include <cstdint>
#include <memory>
#include <vector>

namespace cyane::world::layer {

// Minecraft 的 GenLayer 用一套固定的 LCG 常量做种子派生。
// 所有运算都是 64 位补码回绕（Java long 语义），C++ 侧用 uint64_t 承载避免 UB。
inline constexpr std::uint64_t kLcgMultiplier = 6364136223846793005ULL;
inline constexpr std::uint64_t kLcgIncrement = 1442695040888963407ULL;

// 单步 LCG：seed * (seed * A + C) + addend，等价于 Java 的
//   seed *= seed * 6364136223846793005L + 1442695040888963407L; seed += addend;
[[nodiscard]] constexpr std::uint64_t lcg_step(std::uint64_t seed,
                                               std::uint64_t addend) noexcept {
    return seed * (seed * kLcgMultiplier + kLcgIncrement) + addend;
}

// GenLayer 的种子状态机：baseSeed（构造期派生）/ worldGenSeed（initWorldGenSeed）/
// chunkSeed（initChunkSeed）。逐行对应 net.minecraft.world.gen.layer.GenLayer。
class LayerSeed {
public:
    // GenLayer(long baseSeedIn)：构造期把 baseSeed 自身回绕三次
    explicit constexpr LayerSeed(std::int64_t base_seed) noexcept {
        std::uint64_t value = static_cast<std::uint64_t>(base_seed);
        const auto seed = static_cast<std::uint64_t>(base_seed);
        value = lcg_step(value, seed);
        value = lcg_step(value, seed);
        value = lcg_step(value, seed);
        base_seed_ = value;
    }

    [[nodiscard]] constexpr std::uint64_t base_seed() const noexcept { return base_seed_; }
    [[nodiscard]] constexpr std::uint64_t world_gen_seed() const noexcept { return world_gen_seed_; }
    [[nodiscard]] constexpr std::uint64_t chunk_seed() const noexcept { return chunk_seed_; }

    // initWorldGenSeed(seed)：父层先初始化，然后自身回绕两次并加 baseSeed
    constexpr void init_world_gen_seed(std::int64_t seed) noexcept {
        world_gen_seed_ = static_cast<std::uint64_t>(seed);
        world_gen_seed_ = lcg_step(world_gen_seed_, base_seed_);
        world_gen_seed_ = lcg_step(world_gen_seed_, base_seed_);
    }

    // initChunkSeed(x, z)：回绕四次，交替加 x / z
    constexpr void init_chunk_seed(std::int64_t x, std::int64_t z) noexcept {
        const auto cx = static_cast<std::uint64_t>(x);
        const auto cz = static_cast<std::uint64_t>(z);
        chunk_seed_ = world_gen_seed_;
        chunk_seed_ = lcg_step(chunk_seed_, cx);
        chunk_seed_ = lcg_step(chunk_seed_, cz);
        chunk_seed_ = lcg_step(chunk_seed_, cx);
        chunk_seed_ = lcg_step(chunk_seed_, cz);
    }

    // nextInt(bound)：(chunkSeed >> 24) % bound，负数补正，然后再推进一次 chunkSeed
    [[nodiscard]] constexpr std::int32_t next_int(std::int32_t bound) noexcept {
        auto value = static_cast<std::int64_t>((chunk_seed_ >> 24) % static_cast<std::uint64_t>(bound));
        if (value < 0) {
            value += bound;
        }
        chunk_seed_ = lcg_step(chunk_seed_, world_gen_seed_);
        return static_cast<std::int32_t>(value);
    }

    // selectRandom：按 nextInt 从候选里取一个
    [[nodiscard]] constexpr std::int32_t select_random(const std::vector<std::int32_t>& options) {
        if (options.empty()) {
            return 0;
        }
        return options[static_cast<std::size_t>(
            next_int(static_cast<std::int32_t>(options.size())))];
    }

    // selectModeOrRandom：多数值优先，否则随机。
    // 注意每条"三缺一"分支都带不等式守卫（Java 里的 p3 != p4 等）——
    // 少了守卫会把两两相等的四元组错误地收敛到 a。
    [[nodiscard]] std::int32_t select_mode_or_random(std::int32_t a, std::int32_t b,
                                                    std::int32_t c, std::int32_t d) {
        if (b == c && c == d) return b;
        if (a == b && a == c) return a;
        if (a == b && a == d) return a;
        if (a == c && a == d) return a;
        if (a == b && c != d) return a;
        if (a == c && b != d) return a;
        if (a == d && b != c) return a;
        if (b == c && a != d) return b;
        if (b == d && a != c) return b;
        if (c == d && a != b) return c;
        return select_random({a, b, c, d});
    }

private:
    std::uint64_t base_seed_{0};
    std::uint64_t world_gen_seed_{0};
    std::uint64_t chunk_seed_{0};
};

// GenLayer 抽象基类。getInts 返回 areaWidth*areaHeight 的层数据（行主序，
// 下标 = x + y*width），与 net.minecraft.world.gen.layer.GenLayer 一致。
class GenLayer {
public:
    explicit GenLayer(std::int64_t base_seed) : seed_(base_seed) {}
    virtual ~GenLayer() = default;

    GenLayer(const GenLayer&) = delete;
    GenLayer& operator=(const GenLayer&) = delete;

    // initWorldGenSeed：父层先初始化，然后自身
    void init_world_gen_seed(std::int64_t seed) {
        if (parent_) {
            parent_->init_world_gen_seed(seed);
        }
        seed_.init_world_gen_seed(seed);
    }

    [[nodiscard]] virtual std::vector<std::int32_t> get_ints(std::int32_t area_x,
                                                             std::int32_t area_y,
                                                             std::int32_t width,
                                                             std::int32_t height) = 0;

protected:
    // 子类可覆盖（GenLayerFuzzyZoom 把众数选择换成纯随机）
    [[nodiscard]] virtual std::int32_t select_mode_or_random(std::int32_t a, std::int32_t b,
                                                             std::int32_t c, std::int32_t d) {
        return seed_.select_mode_or_random(a, b, c, d);
    }

    [[nodiscard]] std::int32_t select_random(const std::vector<std::int32_t>& options) {
        return seed_.select_random(options);
    }

    LayerSeed seed_;
    std::shared_ptr<GenLayer> parent_;
};

}  // namespace cyane::world::layer
