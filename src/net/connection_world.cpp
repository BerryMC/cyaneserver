#include "cyane/net/connection.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

#include "connection_detail.hpp"
#include "cyane/net/packet_writers.hpp"
#include "cyane/core/log.hpp"
#include "cyane/item/item_traits.hpp"
#include "cyane/world/block_drops.hpp"
#include "cyane/world/block_sounds.hpp"
#include "cyane/world/mob_types.hpp"
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
    // 创造模式左键即刻破坏(status 0)；生存模式挖掘完成(status 2)才破坏；旁观不可破坏
    const bool creative = context_.game_mode == proto::game_mode::kCreative;
    const bool spectator = context_.game_mode == proto::game_mode::kSpectator;
    const bool destroy = !spectator && (creative ? (*status == 0) : (*status == 2));
    const std::int32_t bx = position_x(*packed);
    const std::int32_t by = position_y(*packed);
    const std::int32_t bz = position_z(*packed);
    // 生存模式：START(0) 出现裂纹、ABORT(1) 清除裂纹，让附近玩家看到挖掘过程
    if (!creative && (*status == 0 || *status == 1) && context_.hub != nullptr) {
        const auto cpos = world::ChunkPos::from_world(bx, bz);
        if (cpos) {
            ByteWriter out;
            writers::write_block_break_animation(out, player_id_, bx, by, bz,
                                                 *status == 0 ? 0 : 0xFF);
            const std::int32_t radius = std::clamp(context_.view_distance, 2, 8);
            context_.hub->broadcast_near(cpos->x, cpos->z, radius, player_id_,
                                         proto::play_cb::kBlockBreakAnimation, out.data());
        }
    }
    if (!destroy) {
        return true;
    }
    // 破坏前记录原方块，用于生成掉落物（生存模式且非空气）
    const std::uint16_t prev = context_.world != nullptr ? context_.world->block_at(bx, by, bz)
                                                         : world::kStateAir;
    const std::int64_t bkey = detail::block_key(bx, by, bz);
    // 容器方块（箱子/熔炉）：破坏时内容必须掉落并从存储移除，
    // 否则旧条目会随同位置新方块"复活"（幽灵物品/幽灵熔炉）
    if (context_.item_drops != nullptr && prev != world::kStateAir) {
        const auto prev_id = world::block_id(prev);
        if (context_.containers != nullptr && prev_id == world::block_id(world::kStateChest)) {
            const auto chest = context_.containers->snapshot(bkey);
            context_.containers->remove(bkey);
            for (const auto& stack : chest) {
                if (!stack.empty()) {
                    drop_stack(bx + 0.5, by + 0.25, bz + 0.5, stack, bx, bz);
                }
            }
        }
        if (context_.furnaces != nullptr && prev_id == world::block_id(world::kStateFurnace)) {
            const auto state = context_.furnaces->snapshot(bkey);
            context_.furnaces->remove(bkey);
            for (const auto& stack : {state.input, state.fuel, state.output}) {
                if (!stack.empty()) {
                    drop_stack(bx + 0.5, by + 0.25, bz + 0.5, stack, bx, bz);
                }
            }
            if (furnace_open_ && open_furnace_key_ == bkey) {
                furnace_open_ = false;
                open_furnace_key_ = 0;
                close_client_window(kFurnaceWindowId);
            }
        }
        if (context_.crafting_tables != nullptr &&
            prev_id == world::block_id(world::kStateCraftingTable)) {
            const auto grid = context_.crafting_tables->cells(bkey);
            context_.crafting_tables->remove(bkey);
            for (const auto& stack : grid) {
                if (!stack.empty()) {
                    drop_stack(bx + 0.5, by + 0.25, bz + 0.5, stack, bx, bz);
                }
            }
            if (table_open_ && open_table_key_ == bkey) {
                table_open_ = false;
                open_table_key_ = 0;
                close_client_window(kCraftingTableWindowId);
            }
        }
        if (chest_open_ && open_chest_key_ == bkey) {
            chest_open_ = false;
            open_chest_key_ = 0;
            close_client_window(kChestWindowId);
        }
        // 小容器：破坏时内容掉落、从存储移除、打开中则关窗
        if (context_.containers != nullptr && context_.containers->small_exists(bkey)) {
            const auto small = context_.containers->snapshot_small(bkey);
            const std::size_t limit = small.kind == ContainerStore::SmallKind::hopper
                                          ? ContainerStore::kHopperSlots
                                          : ContainerStore::kSmallSlots;
            for (std::size_t slot = 0; slot < limit; ++slot) {
                if (!small.slots[slot].empty()) {
                    drop_stack(bx + 0.5, by + 0.25, bz + 0.5, small.slots[slot], bx, bz);
                }
            }
            if (small_open_ && open_small_key_ == bkey) {
                small_open_ = false;
                open_small_key_ = 0;
                small_slots_ = 0;
                close_client_window(kSmallWindowId);
            }
            context_.containers->remove_small(bkey);
        }
    }
    set_block_and_broadcast(bx, by, bz, world::kStateAir);
    // 门双半块：破坏任一半同步清除另一半
    {
        const auto prev_id = world::block_id(prev);
        if (world::is_door(prev_id)) {
            const auto prev_meta = world::state_meta(prev);
            const std::int32_t other_y = (prev_meta & 0x08) != 0 ? by - 1 : by + 1;
            const auto other = context_.world != nullptr ? context_.world->block_at(bx, other_y, bz)
                                                         : world::kStateAir;
            if (world::block_id(other) == prev_id) {
                set_block_and_broadcast(bx, other_y, bz, world::kStateAir);
            }
        }
    }
    // 附着型方块（按钮/拉杆）：支撑方块没了就一起破坏并掉落
    break_unsupported_neighbors(bx, by, bz);
    if (!creative && context_.item_drops != nullptr && prev != world::kStateAir) {
        // 方块特性掉落表 + 采集资格：无正确工具时方块破坏但不掉落（vanilla 行为）
        const auto tool = item::tool_of(inventory_.hotbar_item(selected_slot_).id);
        for (const auto& drop : world::block_drops(prev, tool)) {
            drop_stack(bx + 0.5, by + 0.25, bz + 0.5,
                       item::ItemStack{drop.item_id, drop.count, drop.damage}, bx, bz);
        }
    }
    return true;
}

void Connection::drop_stack(double x, double y, double z, item::ItemStack stack, std::int32_t bx,
                            std::int32_t bz) {
    if (context_.item_drops == nullptr || stack.empty()) {
        return;
    }
    const std::uint32_t eid = context_.item_drops->spawn(x, y, z, stack, now_ms_);
    if (eid == 0) {
        return;
    }
    const DroppedItem drop{eid, x, y, z, stack, now_ms_};
    spawn_dropped_item(drop);
    if (context_.hub == nullptr) {
        return;
    }
    const auto cpos = world::ChunkPos::from_world(bx, bz);
    if (!cpos) {
        return;
    }
    const std::int32_t radius = std::clamp(context_.view_distance, 2, 8);
    ByteWriter spawn;
    ByteWriter meta;
    encode_dropped_item(drop, spawn, meta);
    context_.hub->broadcast_near(cpos->x, cpos->z, radius, player_id_,
                                 proto::play_cb::kSpawnObject, spawn.data());
    context_.hub->broadcast_near(cpos->x, cpos->z, radius, player_id_,
                                 proto::play_cb::kEntityMetadata, meta.data());
}

bool Connection::handle_play_block_place(ByteSpan payload) {
    // PlayerBlockPlacement (0x1F)：position(i64) | byte face(-1..5) | 其后字段本阶段忽略
    ByteReader reader{payload};
    auto packed = reader.i64();
    auto face = reader.u8();
    if (!packed || !face) {
        return false;
    }
    const std::int32_t cx = position_x(*packed);
    const std::int32_t cy = position_y(*packed);
    const std::int32_t cz = position_z(*packed);
    // 旁观模式无法与方块交互
    if (context_.game_mode == proto::game_mode::kSpectator) {
        return true;
    }
    // 右键点到已有箱子/熔炉/工作台：打开对应窗口而非放置
    if (context_.world != nullptr) {
        const auto clicked = world::block_id(context_.world->block_at(cx, cy, cz));
        log::debug("place at ({},{},{}) face={} clicked_id={} held={}", cx, cy, cz, *face, clicked,
                   inventory_.hotbar_item(selected_slot_).id);
        if (context_.containers != nullptr && clicked == world::block_id(world::kStateChest)) {
            open_chest(detail::block_key(cx, cy, cz));
            return true;
        }
        if (context_.furnaces != nullptr && clicked == world::block_id(world::kStateFurnace)) {
            open_furnace(detail::block_key(cx, cy, cz));
            return true;
        }
        if (context_.crafting_tables != nullptr &&
            clicked == world::block_id(world::kStateCraftingTable)) {
            open_crafting_table(detail::block_key(cx, cy, cz));
            return true;
        }
        // 可切换方块：拉杆/活板门/栅栏门/木门——翻转 meta 开关位
        if (context_.world != nullptr && !sneaking_) {
            const auto clicked_state = context_.world->block_at(cx, cy, cz);
            const auto cid = world::block_id(clicked_state);
            std::uint16_t toggle_bit = 0;
            // 拉杆 69：0x8 = 拉下；按钮 77/143：0x8 = 按下（延迟回弹）；
            // 活板门 96：0x4 = 开；栅栏门 107：0x4 = 开；木门：0x4 = 开（上下半同翻）
            if (cid == 69) {
                toggle_bit = 0x8;
            } else if (world::is_button(cid)) {
                // 已按下的按钮：vanilla 直接忽略（不自作回弹）
                if ((clicked_state & 0x08) != 0) {
                    return true;
                }
                toggle_bit = 0x8;
                // 回弹交给世界级调度：按下的玩家中途断线也要弹起来
                const auto delay = cid == 77 ? 20u : 15u;  // 石 1s / 木 0.75s（vanilla 1.12.2）
                if (context_.block_ticks != nullptr) {
                    context_.block_ticks->schedule_button_release(cx, cy, cz, now_ms_ + delay * 50);
                }
            } else if (cid == 96 || cid == 107) {
                toggle_bit = 0x4;
            } else if (world::is_wooden_door(cid)) {
                // 门由上下两个半块组成，meta 0x8 标上半——找另一半一起翻
                const auto this_meta = world::state_meta(clicked_state);
                const std::int32_t other_y = (this_meta & 0x8) != 0 ? cy - 1 : cy + 1;
                const auto other_state = context_.world->block_at(cx, other_y, cz);
                if (world::block_id(other_state) == cid) {
                    const auto other_open = world::state_meta(other_state) ^ 0x4;
                    set_block_and_broadcast(cx, other_y, cz,
                                            static_cast<std::uint16_t>((cid << 4) | other_open));
                }
                toggle_bit = 0x4;
            }
            if (toggle_bit != 0) {
                const auto new_meta = world::state_meta(clicked_state) ^ toggle_bit;
                const bool turning_on = (new_meta & toggle_bit) != 0;
                set_block_and_broadcast(cx, cy, cz,
                                        static_cast<std::uint16_t>((cid << 4) | new_meta));
                send_block_sound(cx, cy, cz, cid, turning_on);
                return true;
            }
        }
        // 小容器：发射器/投掷器/漏斗
        if (context_.containers != nullptr) {
            if (clicked == world::block_id(world::kStateDispenser)) {
                open_small_container(detail::block_key(cx, cy, cz),
                                     ContainerStore::SmallKind::dispenser);
                return true;
            }
            if (clicked == world::block_id(world::kStateDropper)) {
                open_small_container(detail::block_key(cx, cy, cz),
                                     ContainerStore::SmallKind::dropper);
                return true;
            }
            if (clicked == world::block_id(world::kStateHopper)) {
                open_small_container(detail::block_key(cx, cy, cz),
                                     ContainerStore::SmallKind::hopper);
                return true;
            }
        }
    }
    const item::ItemStack& held = inventory_.hotbar_item(selected_slot_);
    // 刷怪蛋：点击面外侧的格子生成生物（damage 即实体类型 id）
    if (const auto egg = world::spawn_egg_type(held.id, held.damage)) {
        const auto delta = world::face_delta(*face);
        if (spawn_mob_at(*egg, static_cast<double>(cx + delta.dx) + 0.5,
                         static_cast<double>(cy + delta.dy),
                         static_cast<double>(cz + delta.dz) + 0.5, player_pos_.yaw + 180.0f)) {
            consume_held_item();
        }
        return true;
    }
    // 手持食物且未满血：进食优先于放置（1.12 右键食物即食用）
    if (item::food_heal(held.id) && health_ < 20.0f) {
        (void)eat_held_food();
        return true;
    }
    std::uint16_t state = world::block_state_from_item(held.id, held.damage);
    if (state == world::kStateAir) {
        return true;  // 空手或非方块物品：忽略
    }
    // 贴面方块（按钮/拉杆等）meta 编码贴合面——否则客户端渲染在错误朝向
    {
        const auto sid = world::block_id(state);
        if (sid == 77 || sid == 143 || sid == 69) {
            // 1.12.2 按钮/拉杆 meta：0=贴天花板 1=贴地板 2-5=墙面（南北西东）。
            // 点击面决定按钮贴在哪个面：顶/底直接映射；墙面取对面
            // （点北面→按钮贴在北侧→朝南→meta 4；点南→朝北→meta 3，
            //  点西→朝东→meta 5；点东→朝西→meta 2）
            // Cuberite BlockMetaDataToBlockFace 权威映射：
            // meta 0=贴底 1=贴东面 2=贴西面 3=贴南面 4=贴北面 5/6=贴地板
            // 点击面→meta：底→0 顶→5 北→4 南→3 西→2 东→1
            static constexpr std::uint8_t face_meta[] = {0, 5, 4, 3, 2, 1};
            state = static_cast<std::uint16_t>((sid << 4) | face_meta[*face & 0x07]);
        } else if (sid == 96 || sid == 107) {
            // 活板门/栅栏门：低 2 位朝向 = 点击面（0-3：南北西东）
            state = static_cast<std::uint16_t>((sid << 4) | (*face & 0x03));
        }
    }
    const auto delta = world::face_delta(*face);
    const std::int32_t tx = cx + delta.dx;
    const std::int32_t ty = cy + delta.dy;
    const std::int32_t tz = cz + delta.dz;
    // 原版语义：目标格必须为空（本阶段不支持替换型方块），拒绝时回滚且不消耗物品。
    // face=-1（点击点在方块内部）时目标格即被点方块本身，天然被此检查拦住。
    const std::uint16_t target = context_.world != nullptr ? context_.world->block_at(tx, ty, tz)
                                                            : world::kStateAir;
    if (target != world::kStateAir) {
        ByteWriter rollback;
        rollback.position(tx, ty, tz);
        rollback.varint(static_cast<std::int32_t>(target));
        send_packet(proto::play_cb::kBlockChange, rollback.data());
        return true;
    }
    // 原版会取消放置到会挤压任意玩家（含自己）的格子：把方块放进玩家碰撞体 → 直接踢出
    if (context_.hub != nullptr && context_.hub->block_intersects_any_player(tx, ty, tz)) {
        ByteWriter rollback;
        rollback.position(tx, ty, tz);
        rollback.varint(static_cast<std::int32_t>(target));
        send_packet(proto::play_cb::kBlockChange, rollback.data());
        return true;
    }
    // 门（木门 64/193-197、铁门 71）是双半方块：下半 + 上半（meta 0x8 标上半）
    if (world::is_door(world::block_id(state))) {
        const auto did = world::block_id(state);
        const auto facing = world::state_meta(state) & 0x03;
        set_block_and_broadcast(tx, ty, tz, static_cast<std::uint16_t>((did << 4) | facing));
        // 上半：meta 0x8 标上半 + 同朝向
        set_block_and_broadcast(tx, ty + 1, tz, static_cast<std::uint16_t>((did << 4) | 0x08 | facing));
        // 上半放置后跳过后续通用放置（已处理）
        consume_held_item();
        return true;
    }
    set_block_and_broadcast(tx, ty, tz, state);
    // 放下的是箱子/熔炉/小容器：登记对应容器状态
    if (context_.containers != nullptr) {
        const auto placed_id = world::block_id(state);
        if (placed_id == world::block_id(world::kStateChest)) {
            context_.containers->ensure(detail::block_key(tx, ty, tz));
        } else if (placed_id == world::block_id(world::kStateDispenser)) {
            context_.containers->ensure_small(detail::block_key(tx, ty, tz),
                                              ContainerStore::SmallKind::dispenser);
        } else if (placed_id == world::block_id(world::kStateDropper)) {
            context_.containers->ensure_small(detail::block_key(tx, ty, tz),
                                              ContainerStore::SmallKind::dropper);
        } else if (placed_id == world::block_id(world::kStateHopper)) {
            context_.containers->ensure_small(detail::block_key(tx, ty, tz),
                                              ContainerStore::SmallKind::hopper);
        }
    }
    if (context_.furnaces != nullptr &&
        world::block_id(state) == world::block_id(world::kStateFurnace)) {
        context_.furnaces->ensure(detail::block_key(tx, ty, tz));
    }
    // 生存模式消耗一个手持方块并回发该槽（创造模式无限）
    consume_held_item();
    return true;
}

bool Connection::eat_held_food() {
    const item::ItemStack& held = inventory_.hotbar_item(selected_slot_);
    const auto heal = item::food_heal(held.id);
    if (!heal || health_ >= 20.0f || held.empty()) {
        return false;
    }
    // 消耗 1 个（创造模式不消耗）
    consume_held_item();
    health_ = std::min(20.0f, health_ + static_cast<float>(*heal));
    cyane::ByteWriter out;
    out.f32(health_);
    out.varint(20);
    out.f32(5.0f);
    send_packet(proto::play_cb::kUpdateHealth, out.data());
    return true;
}

bool Connection::handle_play_use_item(ByteSpan payload) {
    (void)payload;  // 1.12.2: varint hand，只关心手持物品
    const item::ItemStack& held = inventory_.hotbar_item(selected_slot_);
    // 刷怪蛋：对空使用 → 生成在视线前方 1.5 格（vanilla 是射线打到地面处，简化等价）
    if (const auto egg = world::spawn_egg_type(held.id, held.damage)) {
        constexpr double kDegToRad = 3.14159265358979323846 / 180.0;
        const double yaw = static_cast<double>(player_pos_.yaw) * kDegToRad;
        if (spawn_mob_at(*egg, player_pos_.x - std::sin(yaw) * 1.5, player_pos_.y,
                         player_pos_.z + std::cos(yaw) * 1.5, player_pos_.yaw + 180.0f)) {
            consume_held_item();
        }
        return true;
    }
    (void)eat_held_food();  // 吃不下（满血/非食物）不算协议错误——返回 false 会断连
    return true;
}

void Connection::send_block_sound(std::int32_t x, std::int32_t y, std::int32_t z,
                                  std::uint16_t block_id, bool on) {
    const auto sound = world::toggle_sound(block_id);
    if (!sound) {
        return;
    }
    ByteWriter out;
    writers::write_named_sound(out, on ? sound->on_id : sound->off_id, proto::sound_category::kBlocks,
                               x, y, z, sound->volume, on ? sound->on_pitch : sound->off_pitch);
    // 客户端已本地预测的音效不再回发给操作者（否则双响）
    if (!(on ? sound->on_predicted : sound->off_predicted)) {
        send_packet(proto::play_cb::kSoundEffect, out.data());
    }
    // 附近玩家始终要听到（自己上面可能已单发，这里的 exclude 是自己）
    if (context_.hub != nullptr) {
        if (const auto cpos = world::ChunkPos::from_world(x, z)) {
            const std::int32_t radius = std::max(1, sound->radius / 16);
            context_.hub->broadcast_near(cpos->x, cpos->z, radius, player_id_,
                                         proto::play_cb::kSoundEffect, out.data());
        }
    }
}

void Connection::break_unsupported_neighbors(std::int32_t x, std::int32_t y, std::int32_t z) {
    if (context_.world == nullptr) {
        return;
    }
    // 六个方向：只有支撑指向 (x,y,z) 的按钮/拉杆会掉（vanilla 邻接更新里同判）
    static constexpr std::array<world::BlockFaceDelta, 6> kNeighbors{{{-1, 0, 0}, {1, 0, 0},
                                                                      {0, -1, 0}, {0, 1, 0},
                                                                      {0, 0, -1}, {0, 0, 1}}};
    for (const auto& offset : kNeighbors) {
        const std::int32_t nx = x + offset.dx;
        const std::int32_t ny = y + offset.dy;
        const std::int32_t nz = z + offset.dz;
        const auto state = context_.world->block_at(nx, ny, nz);
        const auto id = world::block_id(state);
        if (!world::is_button(id) && !world::is_lever(id)) {
            continue;
        }
        // 只处理支撑正好是破坏点 (x,y,z) 的那些方块
        if (!world::supported_by(state, x - nx, y - ny, z - nz)) {
            continue;
        }
        set_block_and_broadcast(nx, ny, nz, world::kStateAir);
        // vanilla 走方块自身的 dropBlock：与环境破坏同理，不受创造模式影响
        if (context_.item_drops != nullptr) {
            for (const auto& drop : world::block_drops(state, item::ToolInfo{})) {
                drop_stack(nx + 0.5, ny + 0.25, nz + 0.5,
                           item::ItemStack{drop.item_id, drop.count, drop.damage}, nx, nz);
            }
        }
    }
}

void Connection::send_chunk(world::ChunkPos pos) {
    const world::Chunk chunk = context_.world != nullptr ? context_.world->chunk_at(pos)
                                                         : world::make_flat_chunk(pos);
    cyane::ByteWriter fields;
    world::write_full_chunk(fields, chunk);
    send_packet(proto::play_cb::kChunkData, fields.data());
    loaded_chunks_.insert(detail::chunk_key(pos));
}

void Connection::send_pending_chunks(std::size_t limit) {
    while (limit != 0 && !pending_chunks_.empty()) {
        const auto pos = pending_chunks_.front();
        pending_chunks_.pop_front();
        pending_chunk_keys_.erase(detail::chunk_key(pos));
        send_chunk(pos);
        send_chunk_entities(pos.x, pos.z);
        --limit;
    }
}

void Connection::unload_chunk(world::ChunkPos pos) {
    const std::int64_t key = detail::chunk_key(pos);
    // 尚未发出的待发现块：直接从待发表移除，不发 UnloadChunk（客户端没见过它）
    if (!loaded_chunks_.contains(key)) {
        pending_chunk_keys_.erase(key);
        std::erase_if(pending_chunks_, [&](const world::ChunkPos& p) {
            return detail::chunk_key(p) == key;
        });
        return;
    }
    // UnloadChunk (0x1D)：int chunkX | int chunkZ
    cyane::ByteWriter fields;
    fields.i32(pos.x);
    fields.i32(pos.z);
    send_packet(proto::play_cb::kUnloadChunk, fields.data());
    loaded_chunks_.erase(key);
    // 干净区块从内存释放（脏区块留待落盘），约束常驻内存
    if (context_.world != nullptr) {
        context_.world->release_chunk(pos);
    }
}

void Connection::update_view(world::ChunkPos center) {
    const std::int32_t radius = std::clamp(context_.view_distance, 2, 8);
    // 先入队视距内缺失的区块（由近及远），由 tick 限流发出
    for (std::int32_t r = 0; r <= radius; ++r) {
        for (std::int32_t cx = center.x - r; cx <= center.x + r; ++cx) {
            for (std::int32_t cz = center.z - r; cz <= center.z + r; ++cz) {
                // 只处理当前环（切比雪夫距离 == r），避免重复
                const std::int32_t cheb = std::max(std::abs(cx - center.x), std::abs(cz - center.z));
                if (cheb != r) {
                    continue;
                }
                const world::ChunkPos pos{cx, cz};
                const std::int64_t key = detail::chunk_key(pos);
                if (!loaded_chunks_.contains(key) && pending_chunk_keys_.insert(key).second) {
                    pending_chunks_.push_back(pos);
                }
            }
        }
    }
    // 再卸载超出视距的区块（含尚未发出的待发现块）
    std::vector<world::ChunkPos> stale;
    for (const std::int64_t key : loaded_chunks_) {
        const world::ChunkPos pos{static_cast<std::int32_t>(key >> 32),
                                  static_cast<std::int32_t>(static_cast<std::uint32_t>(key))};
        if (std::max(std::abs(pos.x - center.x), std::abs(pos.z - center.z)) > radius) {
            stale.push_back(pos);
        }
    }
    for (const world::ChunkPos pos : pending_chunks_) {
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
