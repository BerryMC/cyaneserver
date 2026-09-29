#pragma once

#include <cstdint>
#include <mutex>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "cyane/world/blocks.hpp"
#include "cyane/world/chunk.hpp"
#include "cyane/world/chunk_codec.hpp"

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
        const auto* sc = find_locked(*pos);
        if (sc == nullptr) {
            return flat_baseline(wy);
        }
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
    void set_block(std::int32_t wx, std::int32_t wy, std::int32_t wz, std::uint16_t state) {
        if (wy < 0 || wy >= kChunkSizeY) {
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

    // 存档载入：整体替换区块内容（explicit 语义：缺失 section = 空气）。
    // 从磁盘启动载入时 dirty=false（文件即权威，无需回写）；程序化构造传 true。
    // source_nbt 为磁盘原始 NBT，保存时作为无损打补丁的底（空则整体重编码）。
    void load_chunk(ChunkPos pos, Chunk chunk, bool dirty = false, Bytes source_nbt = {}) {
        std::lock_guard<std::mutex> lock{mutex_};
        auto& sc = chunks_[chunk_key(pos)];
        sc.chunk = std::move(chunk);
        sc.dirty = dirty;
        sc.source_nbt = std::move(source_nbt);
    }

    // 区块的磁盘原始 NBT（无则空）
    [[nodiscard]] Bytes source_nbt(ChunkPos pos) const {
        std::lock_guard<std::mutex> lock{mutex_};
        if (const auto* sc = find_locked(pos); sc != nullptr) {
            return sc->source_nbt;
        }
        return {};
    }

    // 释放干净区块（内存上限管理）；脏区块保留至落盘
    void release_chunk(ChunkPos pos) {
        std::lock_guard<std::mutex> lock{mutex_};
        const auto key = chunk_key(pos);
        if (auto it = chunks_.find(key); it != chunks_.end() && !it->second.dirty) {
            chunks_.erase(it);
        }
    }

    [[nodiscard]] std::vector<ChunkPos> dirty_chunks() const {
        std::vector<ChunkPos> out;
        std::lock_guard<std::mutex> lock{mutex_};
        out.reserve(chunks_.size());
        for (const auto& [key, sc] : chunks_) {
            if (sc.dirty) {
                out.push_back(ChunkPos{static_cast<std::int32_t>(key >> 32),
                                       static_cast<std::int32_t>(key & 0xFFFFFFFFll)});
            }
        }
        return out;
    }

    void clear_dirty(ChunkPos pos) {
        std::lock_guard<std::mutex> lock{mutex_};
        if (auto it = chunks_.find(chunk_key(pos)); it != chunks_.end()) {
            it->second.dirty = false;
        }
    }

    [[nodiscard]] std::size_t loaded_chunks() const {
        std::lock_guard<std::mutex> lock{mutex_};
        return chunks_.size();
    }

private:
    struct StoredChunk {
        Chunk chunk;
        Bytes source_nbt;  // 磁盘原始 NBT；无损保存时打补丁的底
        bool dirty{false};
    };

    [[nodiscard]] static std::int64_t chunk_key(ChunkPos pos) noexcept {
        return (static_cast<std::int64_t>(pos.x) << 32) | static_cast<std::uint32_t>(pos.z);
    }

    [[nodiscard]] const StoredChunk* find_locked(ChunkPos pos) const {
        const auto it = chunks_.find(chunk_key(pos));
        return it != chunks_.end() ? &it->second : nullptr;
    }

    [[nodiscard]] StoredChunk& ensure_locked(ChunkPos pos) {
        const auto key = chunk_key(pos);
        if (auto it = chunks_.find(key); it != chunks_.end()) {
            return it->second;
        }
        StoredChunk sc;
        sc.chunk = materialize_flat(pos);
        return chunks_.emplace(key, std::move(sc)).first->second;
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
    std::unordered_map<std::int64_t, StoredChunk> chunks_;
};

}