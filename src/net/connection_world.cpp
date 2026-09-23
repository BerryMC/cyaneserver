#include "cyane/net/connection.hpp"

#include <algorithm>
#include <vector>

#include "connection_detail.hpp"
#include "cyane/world/chunk_codec.hpp"

namespace cyane::net {

void Connection::set_block_and_broadcast(std::int32_t wx, std::int32_t wy, std::int32_t wz,
                                         std::uint16_t state) {
    if (context_.world != nullptr) {
        context_.world->set_block(wx, wy, wz, state);
    }
    // BlockChange (0x0B)：position(i64) | varint blockStateId
    ByteWriter change;
    change.position(wx, wy, wz);
    change.varint(static_cast<std::int32_t>(state));
    send_packet(proto::play_cb::kBlockChange, change.data());
    if (context_.hub != nullptr) {
        const auto cpos = world::ChunkPos::from_world(wx, wz);
        if (cpos) {
            const std::int32_t radius = std::clamp(context_.view_distance, 2, 8);
            context_.hub->broadcast_near(cpos->x, cpos->z, radius, player_id_,
                                         proto::play_cb::kBlockChange, change.data());
        }
    }
}

bool Connection::handle_play_digging(ByteSpan payload) {
    // PlayerDigging (0x14)：varint status | position(i64) | byte face
    ByteReader reader{payload};
    auto status = reader.varint();
    auto packed = reader.i64();
    auto face = reader.u8();
    if (!status || !packed || !face) {
        return false;
    }
    // 创造模式左键即刻破坏(status 0)；生存模式挖掘完成(status 2)才破坏
    const bool creative = context_.game_mode == proto::game_mode::kCreative;
    const bool destroy = creative ? (*status == 0) : (*status == 2);
    if (!destroy) {
        return true;
    }
    const std::int32_t bx = position_x(*packed);
    const std::int32_t by = position_y(*packed);
    const std::int32_t bz = position_z(*packed);
    // 破坏前记录原方块，用于生成掉落物（生存模式且非空气）
    const std::uint16_t prev = context_.world != nullptr ? context_.world->block_at(bx, by, bz)
                                                         : world::kStateAir;
    set_block_and_broadcast(bx, by, bz, world::kStateAir);
    if (!creative && context_.item_drops != nullptr && prev != world::kStateAir) {
        // 方块 → 掉落物：1.12.2 方块 id<256 与物品 id 同值，meta 作 damage
        const std::int16_t item_id = static_cast<std::int16_t>(world::block_id(prev));
        const std::int16_t dmg = static_cast<std::int16_t>(world::state_meta(prev));
        const double dx = bx + 0.5;
        const double dy = by + 0.25;
        const double dz = bz + 0.5;
        const item::ItemStack stack{item_id, 1, dmg};
        const std::uint32_t eid = context_.item_drops->spawn(dx, dy, dz, stack, now_ms_);
        if (eid != 0) {
            const DroppedItem drop{eid, dx, dy, dz, stack, now_ms_};
            spawn_dropped_item(drop);
            // 向附近其他玩家广播掉落物（SpawnObject + EntityMetadata 二连）
            if (context_.hub != nullptr) {
                const auto cpos = world::ChunkPos::from_world(bx, bz);
                if (cpos) {
                    const std::int32_t radius = std::clamp(context_.view_distance, 2, 8);
                    ByteWriter spawn;
                    ByteWriter meta;
                    encode_dropped_item(drop, spawn, meta);
                    context_.hub->broadcast_near(cpos->x, cpos->z, radius, player_id_,
                                                 proto::play_cb::kSpawnObject, spawn.data());
                    context_.hub->broadcast_near(cpos->x, cpos->z, radius, player_id_,
                                                 proto::play_cb::kEntityMetadata, meta.data());
                }
            }
        }
    }
    return true;
}

bool Connection::handle_play_block_place(ByteSpan payload) {
    // PlayerBlockPlacement (0x1F)：position(i64) | varint face | varint hand
    //                             | float cursorX/Y/Z
    ByteReader reader{payload};
    auto packed = reader.i64();
    auto face = reader.varint();
    if (!packed || !face) {
        return false;
    }
    const std::int32_t cx = position_x(*packed);
    const std::int32_t cy = position_y(*packed);
    const std::int32_t cz = position_z(*packed);
    // 右键点到已有箱子：打开容器窗口而非放置
    if (context_.world != nullptr && context_.containers != nullptr &&
        world::block_id(context_.world->block_at(cx, cy, cz)) == world::block_id(world::kStateChest)) {
        open_chest(detail::block_key(cx, cy, cz));
        return true;
    }
    const item::ItemStack& held = inventory_.hotbar_item(selected_slot_);
    const std::uint16_t state = world::block_state_from_item(held.id, held.damage);
    if (state == world::kStateAir) {
        return true;  // 空手或非方块物品：忽略
    }
    const auto delta = world::face_delta(*face);
    const std::int32_t tx = cx + delta.dx;
    const std::int32_t ty = cy + delta.dy;
    const std::int32_t tz = cz + delta.dz;
    // 原版会取消放置到会挤压任意玩家（含自己）的格子：把方块放进玩家碰撞体 → 直接踢出
    if (context_.hub != nullptr && context_.hub->block_intersects_any_player(tx, ty, tz)) {
        // 向放置者回发当前方块状态，让客户端回滚预测
        ByteWriter rollback;
        rollback.position(tx, ty, tz);
        rollback.varint(static_cast<std::int32_t>(context_.world != nullptr
                                                      ? context_.world->block_at(tx, ty, tz)
                                                      : world::kStateAir));
        send_packet(proto::play_cb::kBlockChange, rollback.data());
        return true;
    }
    set_block_and_broadcast(tx, ty, tz, state);
    // 放下的是箱子：在容器存储登记一个空箱
    if (context_.containers != nullptr &&
        world::block_id(state) == world::block_id(world::kStateChest)) {
        context_.containers->ensure(detail::block_key(tx, ty, tz));
    }
    // 生存模式消耗一个手持方块并回发该槽（创造模式无限）
    if (context_.game_mode != proto::game_mode::kCreative) {
        const std::size_t hs = item::PlayerInventory::hotbar_slot(selected_slot_);
        item::ItemStack after = held;
        if (after.count > 0) {
            --after.count;
        }
        if (after.count == 0) {
            after = item::ItemStack::air();
        }
        inventory_.set_slot(hs, after);
        send_slot(0, static_cast<std::int16_t>(hs), after);
    }
    return true;
}

void Connection::send_chunk(world::ChunkPos pos) {
    world::Chunk chunk = context_.world != nullptr ? context_.world->build_chunk(pos)
                                                    : world::make_flat_chunk(pos);
    cyane::ByteWriter fields;
    world::write_full_chunk(fields, chunk);
    send_packet(proto::play_cb::kChunkData, fields.data());
    loaded_chunks_.insert(detail::chunk_key(pos));
}

void Connection::unload_chunk(world::ChunkPos pos) {
    // UnloadChunk (0x1D)：int chunkX | int chunkZ
    cyane::ByteWriter fields;
    fields.i32(pos.x);
    fields.i32(pos.z);
    send_packet(proto::play_cb::kUnloadChunk, fields.data());
    loaded_chunks_.erase(detail::chunk_key(pos));
}

void Connection::update_view(world::ChunkPos center) {
    const std::int32_t radius = std::clamp(context_.view_distance, 2, 8);
    // 先加载视距内缺失的区块（由近及远）
    for (std::int32_t r = 0; r <= radius; ++r) {
        for (std::int32_t cx = center.x - r; cx <= center.x + r; ++cx) {
            for (std::int32_t cz = center.z - r; cz <= center.z + r; ++cz) {
                // 只处理当前环（切比雪夫距离 == r），避免重复
                const std::int32_t cheb = std::max(std::abs(cx - center.x), std::abs(cz - center.z));
                if (cheb != r) {
                    continue;
                }
                const world::ChunkPos pos{cx, cz};
                if (!loaded_chunks_.contains(detail::chunk_key(pos))) {
                    send_chunk(pos);
                }
            }
        }
    }
    // 再卸载超出视距的区块
    std::vector<world::ChunkPos> stale;
    for (const std::int64_t key : loaded_chunks_) {
        const world::ChunkPos pos{static_cast<std::int32_t>(key >> 32),
                                  static_cast<std::int32_t>(static_cast<std::uint32_t>(key))};
        if (std::max(std::abs(pos.x - center.x), std::abs(pos.z - center.z)) > radius) {
            stale.push_back(pos);
        }
    }
    for (const world::ChunkPos pos : stale) {
        unload_chunk(pos);
    }
    last_center_ = center;
    has_center_ = true;
}

}
