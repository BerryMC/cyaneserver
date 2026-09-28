#pragma once

#include <cstdint>
#include <mutex>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

#include "cyane/world/blocks.hpp"
#include "cyane/world/chunk.hpp"
#include "cyane/world/chunk_codec.hpp"

namespace cyane::world {

// 超平坦地形作为 baseline，叠加一张运行期编辑覆盖表。
// 多个 I/O reactor 线程共享同一个 World，故所有访问加锁。
class World {
public:
    World() = default;

    void set_block(std::int32_t wx, std::int32_t wy, std::int32_t wz, std::uint16_t state) {
        const auto key = ChunkPos::from_world(wx, wz);
        if (!key || wy < 0 || wy >= kChunkSizeY) {
            return;
        }
        const std::uint32_t local = local_index(*key, wx, wy, wz);
        std::lock_guard<std::mutex> lock(mutex_);
        edits_[chunk_key(*key)][local] = state;
    }

    // 优先返回编辑覆盖，否则回退到超平坦 baseline
    [[nodiscard]] std::uint16_t block_at(std::int32_t wx, std::int32_t wy, std::int32_t wz) const {
        const auto key = ChunkPos::from_world(wx, wz);
        if (!key || wy < 0 || wy >= kChunkSizeY) {
            return kStateAir;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (auto it = edits_.find(chunk_key(*key)); it != edits_.end()) {
                if (auto b = it->second.find(local_index(*key, wx, wy, wz)); b != it->second.end()) {
                    return b->second;
                }
            }
        }
        return flat_baseline(wy);
    }

    // 超平坦打底后套用该区块已记录的编辑
    [[nodiscard]] Chunk build_chunk(ChunkPos pos) const {
        Chunk chunk = make_flat_chunk(pos);
        std::lock_guard<std::mutex> lock(mutex_);
        if (auto it = edits_.find(chunk_key(pos)); it != edits_.end()) {
            for (const auto& [local, state] : it->second) {
                const std::int32_t wx = pos.world_x() + static_cast<std::int32_t>(local & 0xF);
                const std::int32_t wz = pos.world_z() + static_cast<std::int32_t>((local >> 4) & 0xF);
                const std::int32_t wy = static_cast<std::int32_t>(local >> 8);
                chunk.set_block_state(wx, wy, wz, state);
            }
        }
        return chunk;
    }

    // ---- 持久化接口（Anvil 存档）----

    // 拷出某区块的全部编辑（local 索引 → 状态）
    [[nodiscard]] std::vector<std::pair<std::uint32_t, std::uint16_t>>
    chunk_edits(ChunkPos pos) const {
        std::lock_guard<std::mutex> lock{mutex_};
        if (auto it = edits_.find(chunk_key(pos)); it != edits_.end()) {
            return {it->second.begin(), it->second.end()};
        }
        return {};
    }

    // 合并存档加载来的编辑（同 local 覆盖）
    void merge_edits(ChunkPos pos, std::span<const std::pair<std::uint32_t, std::uint16_t>> edits) {
        std::lock_guard<std::mutex> lock{mutex_};
        auto& target = edits_[chunk_key(pos)];
        for (const auto& [local, state] : edits) {
            target.insert_or_assign(local, state);
        }
    }

    // 有编辑的区块列表（保存时逐区块写 region）
    [[nodiscard]] std::vector<ChunkPos> edited_chunks() const {
        std::vector<ChunkPos> out;
        std::lock_guard<std::mutex> lock{mutex_};
        out.reserve(edits_.size());
        for (const auto& [key, edits] : edits_) {
            if (!edits.empty()) {
                out.push_back(ChunkPos{static_cast<std::int32_t>(key >> 32),
                                       static_cast<std::int32_t>(key & 0xFFFFFFFFll)});
            }
        }
        return out;
    }

private:
    // 区块内线性索引：y<<8 | z<<4 | x
    [[nodiscard]] static std::uint32_t local_index(ChunkPos pos, std::int32_t wx, std::int32_t wy,
                                                   std::int32_t wz) noexcept {
        const auto x = static_cast<std::uint32_t>(wx - pos.world_x());
        const auto z = static_cast<std::uint32_t>(wz - pos.world_z());
        return (static_cast<std::uint32_t>(wy) << 8) | (z << 4) | x;
    }

    [[nodiscard]] static std::int64_t chunk_key(ChunkPos pos) noexcept {
        return (static_cast<std::int64_t>(pos.x) << 32) | static_cast<std::uint32_t>(pos.z);
    }

    mutable std::mutex mutex_;
    std::unordered_map<std::int64_t, std::unordered_map<std::uint32_t, std::uint16_t>> edits_;
};

}
