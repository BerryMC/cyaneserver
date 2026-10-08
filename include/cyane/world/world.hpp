#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "cyane/world/blocks.hpp"
#include "cyane/world/chunk.hpp"
#include "cyane/world/chunk_codec.hpp"
#include "cyane/world/chunk_generator.hpp"

namespace cyane::world {

// 世界存储：每个区块持有全量 section 状态（可从存档载入原版真实地形），
// 尚未物化的区块回退为超平坦 baseline（M5 世界生成接入后由生成器取代）。
// 缺失 section = 全空气；物化超平坦区块时 section 0 以 baseline 填充。
// 多个 I/O reactor 线程共享，全部访问加锁。
class World {
public:
    // 读取方块：未物化区块按 baseline 兜底
    [[nodiscard]] std::uint16_t block_at(std::int32_t wx, std::int32_t wy, std::int32_t wz) {
        if (wy < 0 || wy >= kChunkSizeY) {
            return kStateAir;
        }
        const auto pos = ChunkPos::from_world(wx, wz);
        if (!pos) {
            return kStateAir;
        }
        std::lock_guard<std::mutex> lock{mutex_};
        auto it = chunks_.find(chunk_key(*pos));
        if (it == chunks_.end() && loader_) {
            Chunk loaded{*pos};
            Bytes source;
            if (loader_(*pos, loaded, source)) {
                auto sc = std::make_shared<StoredChunk>();
                sc->chunk = std::move(loaded);
                sc->source_nbt = std::move(source);
                sc->dirty = false;
                it = chunks_.emplace(chunk_key(*pos), std::move(sc)).first;
            }
        }
        if (it == chunks_.end()) {
            return flat_baseline(wy);
        }
        const auto& sc = it->second;
        const auto* section = sc->chunk.section(static_cast<std::size_t>(wy) / 16);
        if (section == nullptr || section->empty()) {
            return kStateAir;
        }
        return section->state(section_index(static_cast<std::size_t>(wx - pos->world_x()),
                                            static_cast<std::size_t>(wy % 16),
                                            static_cast<std::size_t>(wz - pos->world_z())));
    }

    // 修改方块：物化所在区块并标记脏；缺失 section 由 Chunk 按全空气创建
    // （超平坦区块物化时 section 0 已承载 baseline，不会走到该路径）
    void set_block(std::int32_t wx, std::int32_t wy, std::int32_t wz, std::uint16_t state) {        if (wy < 0 || wy >= kChunkSizeY) {
            return;
        }
        const auto pos = ChunkPos::from_world(wx, wz);
        if (!pos) {
            return;
        }
        std::lock_guard<std::mutex> lock{mutex_};
        auto& sc = ensure_locked(*pos);
        sc.chunk.set_block_state(wx, wy, wz, state);
        sc.dirty = true;
    }

    // 发送/编码用：物化（如缺）后返回拷贝；已存在的区块原样返回
    [[nodiscard]] Chunk chunk_at(ChunkPos pos) {
        std::lock_guard<std::mutex> lock{mutex_};
        return ensure_locked(pos).chunk;
    }

    // 设置世界生成器（Server 启动时注入，替换超平坦 baseline）
    void set_generator(std::uint64_t seed) {
        generator_ = std::make_unique<gen::ChunkGeneratorOverworld>(seed);
    }

    // 世界存档：整体替换区块内容（explicit 语义：缺失 section = 空气）。
    // 从磁盘启动载入时 dirty=false（文件即权威，无需回写）；程序化构造传 true。
    // source_nbt 为磁盘原始 NBT，保存时作为无损打补丁的底（空则整体重编码）。
    void load_chunk(ChunkPos pos, Chunk chunk, bool dirty = false, Bytes source_nbt = {}) {
        std::lock_guard<std::mutex> lock{mutex_};
        auto sc = std::make_shared<StoredChunk>();
        sc->chunk = std::move(chunk);
        sc->dirty = dirty;
        sc->source_nbt = std::move(source_nbt);
        chunks_.insert_or_assign(chunk_key(pos), std::move(sc));
    }

    // 区块的磁盘原始 NBT（无则空）
    [[nodiscard]] Bytes source_nbt(ChunkPos pos) const {
        std::lock_guard<std::mutex> lock{mutex_};
        if (const auto* sc = find_locked(pos); sc != nullptr) {
            return sc->source_nbt;
        }
        return {};
    }

    // 有效光照等级：max(sky_light, block_light)，返回 0 表示该位置无区块/无光照数据。
    // 用于生成判定：实体只能在区块内（有区块数据）生成。
    [[nodiscard]] std::uint8_t light_at(std::int32_t wx, std::int32_t wy, std::int32_t wz) const {
        if (wy < 0 || wy >= kChunkSizeY) {
            return 0;
        }
        const auto pos = ChunkPos::from_world(wx, wz);
        if (!pos) {
            return 0;
        }
        std::lock_guard<std::mutex> lock{mutex_};
        const auto* sc = find_locked(*pos);
        if (sc == nullptr) {
            return 0;
        }
        const auto* section = sc->chunk.section(static_cast<std::size_t>(wy) / 16);
        if (section == nullptr || section->empty() ||
            section->sky_light.empty() || section->block_light.empty()) {
            return 0;
        }
        const auto idx = section_index(static_cast<std::size_t>(wx - pos->world_x()),
                                       static_cast<std::size_t>(wy % 16),
                                       static_cast<std::size_t>(wz - pos->world_z()));
        const auto sky = static_cast<std::uint8_t>(section->sky_light[idx >> 1] >> ((idx & 1u) ? 4u : 0u) & 0xF);
        const auto block = static_cast<std::uint8_t>(section->block_light[idx >> 1] >> ((idx & 1u) ? 4u : 0u) & 0xF);
        return static_cast<std::uint8_t>(std::max(sky, block));
    }

    // 按需加载回调：区块不在内存时（被释放后玩家回来）从磁盘重读，
    // 返回 true 表示载入了真实地形。由 WorldPersistence 注入（读 region + 恢复方块实体）。
    // 返回的 chunk 已带 dirty=false 与磁盘 source_nbt。
    using ChunkLoader = std::function<bool(ChunkPos pos, Chunk& out, Bytes& source_nbt)>;

    void set_loader(ChunkLoader loader) { loader_ = std::move(loader); }

    // 出生点列的地表高度：自上而下第一个非空气方块的上一格。
    // 先物化该区块再逐格探测——用于把 level.dat 里过时的 SpawnY 修正到实际地面。
    [[nodiscard]] std::int32_t surface_y(std::int32_t wx, std::int32_t wz) {
        const auto pos = ChunkPos::from_world(wx, wz);
        if (!pos) {
            return 4;
        }
        (void)chunk_at(*pos);  // 物化（载入地形或超平坦 baseline）
        for (std::int32_t wy = kChunkSizeY - 1; wy >= 0; --wy) {
            if (block_at(wx, wy, wz) != kStateAir) {
                return wy + 1;
            }
        }
        return 4;
    }

    // 区块是否在内存中（不触发物化）。保存侧用它区分"真区块"与
    // "被释放后 materialize 出的超平坦假区块"——后者绝不能覆盖磁盘地形。
    [[nodiscard]] bool contains(ChunkPos pos) const {
        std::lock_guard<std::mutex> lock{mutex_};
        return find_locked(pos) != nullptr;
    }

    // 释放干净区块（内存上限管理）；脏区块保留至落盘。
    // shared_ptr 所有权：BlockCache 等锁外读者持副本期间区块不会被析构
    void release_chunk(ChunkPos pos) {
        std::lock_guard<std::mutex> lock{mutex_};
        const auto key = chunk_key(pos);
        if (auto it = chunks_.find(key); it != chunks_.end() && !it->second->dirty) {
            chunks_.erase(it);
        }
    }

    [[nodiscard]] std::vector<ChunkPos> dirty_chunks() const {
        std::vector<ChunkPos> out;
        std::lock_guard<std::mutex> lock{mutex_};
        out.reserve(chunks_.size());
        for (const auto& [key, sc] : chunks_) {
            if (sc->dirty) {
                out.push_back(ChunkPos{static_cast<std::int32_t>(key >> 32),
                                       static_cast<std::int32_t>(key & 0xFFFFFFFFll)});
            }
        }
        return out;
    }

    void clear_dirty(ChunkPos pos) {
        std::lock_guard<std::mutex> lock{mutex_};
        if (auto it = chunks_.find(chunk_key(pos)); it != chunks_.end()) {
            it->second->dirty = false;
        }
    }

    [[nodiscard]] std::size_t loaded_chunks() const {
        std::lock_guard<std::mutex> lock{mutex_};
        return chunks_.size();
    }

    // 世界区块只读快照（对应 vanilla world.ChunkCache）：寻路一类"同一批坐标读上千次"
    // 的只读负载用它。vanilla 的 ChunkCache 只**持有 Chunk 引用**（数组）不复制数据，
    // 这里同样：构造时加锁把覆盖范围内的 section 指针取出来，之后无锁读。
    // 快照同时持有区块的 shared_ptr——锁外读期间区块即便被 release 也只是引用计数
    // 降级，section（Chunk 内定长数组，地址稳定）始终有效。快照不得跨 tick 保存。
    struct StoredChunk {
        Chunk chunk;
        Bytes source_nbt;  // 磁盘原始 NBT；无损保存时打补丁的底
        bool dirty{false};
    };

    class BlockCache {
    public:
        BlockCache(World& world, std::int32_t cx, std::int32_t cz, std::int32_t radius) {
            const auto side = static_cast<std::size_t>(radius * 2 + 1);
            chunks_.assign(side * side, {});
            origin_cx_ = cx;
            origin_cz_ = cz;
            side_ = side;
            radius_ = radius;
            std::lock_guard<std::mutex> lock{world.mutex_};
            for (std::int32_t dx = -radius; dx <= radius; ++dx) {
                for (std::int32_t dz = -radius; dz <= radius; ++dz) {
                    auto owner = world.find_shared_locked(ChunkPos{cx + dx, cz + dz});
                    if (owner == nullptr) {
                        continue;  // 未物化：留空，读时回退 baseline
                    }
                    const auto slot =
                        static_cast<std::size_t>(dx + radius_) * side_ +
                        static_cast<std::size_t>(dz + radius_);
                    auto& cached = chunks_[slot];
                    cached.owner = std::move(owner);
                    for (std::size_t sy = 0; sy < kSectionsPerChunk; ++sy) {
                        const auto* section = cached.owner->chunk.section(sy);
                        if (section != nullptr && !section->empty()) {
                            cached.sections[sy] = section;
                        }
                    }
                }
            }
        }

        [[nodiscard]] std::uint16_t at(std::int32_t wx, std::int32_t wy,
                                        std::int32_t wz) const noexcept {
            if (wy < 0 || wy >= kChunkSizeY) {
                return kStateAir;
            }
            // dx/dz 是相对原点区块的偏移，本就覆盖 -radius..+radius —— 判越界要跟窗口比
            const auto dx = ChunkPos::floor_div(wx, kChunkSizeX) - origin_cx_;
            const auto dz = ChunkPos::floor_div(wz, kChunkSizeZ) - origin_cz_;
            if (dx < -radius_ || dz < -radius_ || dx > radius_ || dz > radius_) {
                return kStateAir;  // 范围外（等价 vanilla ChunkCache 越界 → AIR）
            }
            const auto slot = static_cast<std::size_t>(dx + radius_) * side_ +
                              static_cast<std::size_t>(dz + radius_);
            const auto& cached = chunks_[slot];
            const auto* section = cached.sections[static_cast<std::size_t>(wy) / 16];
            if (section == nullptr) {
                return flat_baseline(wy);  // 空 section：与 block_at 的 baseline 回退同语义
            }
            return section->state(section_index(static_cast<std::size_t>(wx & 15),
                                                static_cast<std::size_t>(wy % 16),
                                                static_cast<std::size_t>(wz & 15)));
        }

    private:
        static constexpr std::size_t kSectionsPerChunk =
            static_cast<std::size_t>(kChunkSizeY) / 16;

        struct CachedChunk {
            std::shared_ptr<const StoredChunk> owner;
            std::array<const Section*, kSectionsPerChunk> sections{};
        };

        std::vector<CachedChunk> chunks_;
        std::int32_t origin_cx_{0};
        std::int32_t origin_cz_{0};
        std::size_t side_{0};
        std::int32_t radius_{0};
    };

    // 覆盖 radius 个区块半径的只读快照（寻路用；越界读为空气，与 vanilla ChunkCache 同）
    [[nodiscard]] BlockCache block_cache(std::int32_t cx, std::int32_t cz,
                                         std::int32_t radius) const {
        return BlockCache{const_cast<World&>(*this), cx, cz, radius};
    }

private:
    [[nodiscard]] static std::int64_t chunk_key(ChunkPos pos) noexcept {
        return (static_cast<std::int64_t>(pos.x) << 32) | static_cast<std::uint32_t>(pos.z);
    }

    [[nodiscard]] const StoredChunk* find_locked(ChunkPos pos) const {
        const auto it = chunks_.find(chunk_key(pos));
        return it != chunks_.end() ? it->second.get() : nullptr;
    }

    [[nodiscard]] StoredChunk* find_locked(ChunkPos pos) noexcept {
        const auto it = chunks_.find(chunk_key(pos));
        return it != chunks_.end() ? it->second.get() : nullptr;
    }

    // BlockCache 构造用：连同所有权一起交出（锁外读期间区块不会被 release 析构）
    [[nodiscard]] std::shared_ptr<const StoredChunk> find_shared_locked(ChunkPos pos) const {
        const auto it = chunks_.find(chunk_key(pos));
        return it != chunks_.end() ? it->second : nullptr;
    }

    [[nodiscard]] StoredChunk& ensure_locked(ChunkPos pos) {
        const auto key = chunk_key(pos);
        if (auto it = chunks_.find(key); it != chunks_.end()) {
            return *it->second;
        }
        // 先试磁盘按需加载（释放区块回归），失败再试世界生成器，最后回退超平坦
        if (loader_) {
            Chunk loaded{pos};
            Bytes source;
            if (loader_(pos, loaded, source)) {
                auto sc = std::make_shared<StoredChunk>();
                sc->chunk = std::move(loaded);
                sc->source_nbt = std::move(source);
                sc->dirty = false;
                return *chunks_.emplace(key, std::move(sc)).first->second;
            }
        }
        if (generator_) {
            auto sc = std::make_shared<StoredChunk>();
            sc->chunk = generator_->generate(pos.x, pos.z);
            sc->dirty = true;
            return *chunks_.emplace(key, std::move(sc)).first->second;
        }
        auto sc = std::make_shared<StoredChunk>();
        sc->chunk = materialize_flat(pos);
        return *chunks_.emplace(key, std::move(sc)).first->second;
    }

    // 超平坦物化：section 0 显式承载 baseline（bedrock/dirt/grass），其余 section 缺失 = 空气
    [[nodiscard]] static Chunk materialize_flat(ChunkPos pos) {
        Chunk chunk{pos};
        chunk.set_section(0, make_section(0));
        return chunk;
    }

    [[nodiscard]] static Section make_section(std::size_t sy) {
        Section section;
        section.states.resize(kSectionBlockCount, kStateAir);
        if (sy == 0) {
            for (std::size_t y = 0; y < 16; ++y) {
                const auto base = flat_baseline(static_cast<std::int32_t>(y));
                for (std::size_t i = y << 8; i < (y << 8) + 256; ++i) {
                    section.states[i] = base;
                }
            }
        }
        return section;
    }    mutable std::mutex mutex_;
    // shared_ptr：release 后 BlockCache 等锁外读者仍可安全持有区块
    std::unordered_map<std::int64_t, std::shared_ptr<StoredChunk>> chunks_;
    ChunkLoader loader_;  // WorldPersistence 注入；ensure_locked 在持锁状态下调用
    std::unique_ptr<gen::ChunkGeneratorOverworld> generator_;  // 世界生成器（M5）
};

}