#include "cyane/net/connection.hpp"

#include <cmath>
#include <random>

#include "cyane/core/log.hpp"
#include "cyane/net/packet_writers.hpp"
#include "cyane/item/item_traits.hpp"
#include "cyane/item/item_tools.hpp"
#include "cyane/world/mob_types.hpp"

namespace cyane::net {

bool Connection::handle_play_client_command(ByteSpan payload) {
    // ClientCommand (0x03)：varint actionId。0=请求重生，1=请求统计
    ByteReader reader{payload};
    auto action = reader.varint();
    if (!action) {
        return false;
    }
    if (*action == 0 && dead_) {
        respawn_player();
    }
    return true;
}

bool Connection::handle_play_use_entity(ByteSpan payload) {
    // UseEntity (0x0A)：varint target | varint type | [type=0 交互时：varint hand + f32 x/y/z]
    // type 1 = ATTACK
    ByteReader reader{payload};
    auto target = reader.varint();
    auto type = reader.varint();
    if (!target || !type) {
        return false;
    }
    if (*type != 1) {  // interact（0）/ interact at（2）：本服务端无交互语义，静默接受
        return true;
    }
    if (context_.game_mode == proto::game_mode::kSpectator || dead_) {
        return true;
    }
    if (context_.mobs == nullptr) {
        return true;
    }
    const auto mob = context_.mobs->by_id(static_cast<std::uint32_t>(*target));
    if (!mob) {
        return true;
    }
    const auto species = world::mob_type(mob->type);
    if (!species) {
        return true;
    }
    // 挥臂动画：操作者先看到自己挥拳，其他人也看到
    ByteWriter anim;
    writers::write_animation(anim, player_id_, 0);
    send_packet(proto::play_cb::kAnimation, anim.data());
    if (context_.hub != nullptr) {
        context_.hub->broadcast(player_id_, proto::play_cb::kAnimation, anim.data());
    }
    // 伤害：手持攻击力（徒手 1.0）；生物掉血/击退/死亡掉落
    const float damage = item::attack_damage(inventory_.hotbar_item(selected_slot_).id);
    const auto hurt = context_.mobs->damage(static_cast<std::uint32_t>(*target), damage,
                                            player_pos_.x, player_pos_.z);
    if (!hurt.found) {
        return true;
    }
    constexpr auto kBlocks = proto::sound_category::kBlocks;
    ByteWriter hurt_status;
    writers::write_entity_status(hurt_status, static_cast<std::uint32_t>(*target), 2);
    ByteWriter hurt_sound;
    writers::write_named_sound(hurt_sound, hurt.died ? species->death_sound : species->hurt_sound,
                               kBlocks, static_cast<std::int32_t>(hurt.x),
                               static_cast<std::int32_t>(hurt.y),
                               static_cast<std::int32_t>(hurt.z), 1.0f, 1.0f);
    broadcast_entity_packet(hurt_status.data(), static_cast<std::int32_t>(hurt.x),
                            static_cast<std::int32_t>(hurt.z));
    send_packet(proto::play_cb::kSoundEffect, hurt_sound.data());
    if (context_.hub != nullptr) {
        const auto cpos = world::ChunkPos::from_world(static_cast<std::int32_t>(hurt.x),
                                                       static_cast<std::int32_t>(hurt.z));
        if (cpos) {
            const std::int32_t radius = std::clamp(context_.view_distance, 2, 8);
            context_.hub->broadcast_near(cpos->x, cpos->z, radius, player_id_,
                                         proto::play_cb::kSoundEffect, hurt_sound.data());
        }
    }
    if (hurt.died) {
        // 掉落表 → 掉落物（生存/创造都掉，vanilla 死亡掉落与击杀者模式无关）
        if (context_.item_drops != nullptr) {
            for (const auto& drop : species->drops) {
                if (drop.item_id == 0) {
                    break;
                }
                static thread_local std::mt19937 drop_engine{std::random_device{}()};
                if (drop.chance_percent < 100 &&
                    std::uniform_int_distribution<int>(1, 100)(drop_engine) > drop.chance_percent) {
                    continue;
                }
                const auto count = drop.max_count > drop.min_count
                                       ? static_cast<std::uint8_t>(std::uniform_int_distribution<int>(
                                             drop.min_count, drop.max_count)(drop_engine))
                                       : drop.min_count;
                if (count == 0) {
                    continue;
                }
                spawn_dropped_item_at(static_cast<double>(hurt.x), hurt.y,
                                      static_cast<double>(hurt.z),
                                      item::ItemStack{drop.item_id, static_cast<std::uint8_t>(count),
                                                      drop.damage});
            }
        }
        ByteWriter destroy;
        const std::uint32_t ids[] = {static_cast<std::uint32_t>(*target)};
        writers::write_destroy_entities(destroy, ids);
        ByteWriter hurt_still;
        (void)hurt_still;
        // 销毁实体也用 EntityStatus 通道？不——DestroyEntities 是独立包，直接广播
        send_packet(proto::play_cb::kDestroyEntities, destroy.data());
        if (context_.hub != nullptr) {
            const auto cpos = world::ChunkPos::from_world(static_cast<std::int32_t>(hurt.x),
                                                           static_cast<std::int32_t>(hurt.z));
            if (cpos) {
                const std::int32_t radius = std::clamp(context_.view_distance, 2, 8);
                context_.hub->broadcast_near(cpos->x, cpos->z, radius, player_id_,
                                             proto::play_cb::kDestroyEntities, destroy.data());
            }
        }
        log::info("{} killed mob {} (type {})", username_, *target, mob->type);
    }
    return true;
}

void Connection::apply_damage(float amount, double from_x, double from_z) {
    if (dead_ || context_.game_mode == proto::game_mode::kCreative) {
        return;  // 创造模式无敌（vanilla 同）
    }
    // 护甲减伤（ArmorUtil.getDamageAfterAbsorb，槽 5..8 = 头/胸/腿/脚）
    int armor = 0;
    for (std::size_t i = 5; i <= 8; ++i) {
        armor += item::armor_points(inventory_.slot(i).id);
    }
    amount = item::damage_after_armor(amount, armor, 0.0f);
    health_ = std::max(0.0f, health_ - amount);
    ByteWriter health;
    health.f32(health_);
    health.varint(20);
    health.f32(5.0f);
    send_packet(proto::play_cb::kUpdateHealth, health.data());
    // 受伤反馈：EntityStatus(2) 给他人；自己靠 UpdateHealth 的红闪 + 受伤音
    if (context_.hub != nullptr) {
        ByteWriter status;
        writers::write_entity_status(status, player_id_, 2);
        context_.hub->broadcast(player_id_, proto::play_cb::kEntityStatus, status.data());
    }
    ByteWriter sound;
    writers::write_named_sound(sound, 367 /*entity.player.hurt*/, proto::sound_category::kBlocks,
                               static_cast<std::int32_t>(player_pos_.x),
                               static_cast<std::int32_t>(player_pos_.y),
                               static_cast<std::int32_t>(player_pos_.z), 1.0f, 1.0f);
    send_packet(proto::play_cb::kSoundEffect, sound.data());
    // 击退：沿攻击者→玩家方向（水平）+ 向上分量
    double dx = player_pos_.x - from_x;
    double dz = player_pos_.z - from_z;
    const double len = std::sqrt(dx * dx + dz * dz);
    if (len > 1e-4) {
        dx = dx / len * 0.4;
        dz = dz / len * 0.4;
    } else {
        dx = 0.0;
        dz = 0.0;
    }
    ByteWriter velocity;
    writers::write_entity_velocity(velocity, player_id_, dx, 0.36, dz);
    send_packet(proto::play_cb::kEntityVelocity, velocity.data());
    if (health_ <= 0.0f) {
        kill_player();
    }
}

void Connection::broadcast_entity_packet(ByteSpan packet, std::int32_t x, std::int32_t z) {
    send_packet(proto::play_cb::kEntityStatus, packet);
    if (context_.hub == nullptr) {
        return;
    }
    const auto cpos = world::ChunkPos::from_world(x, z);
    if (!cpos) {
        return;
    }
    const std::int32_t radius = std::clamp(context_.view_distance, 2, 8);
    context_.hub->broadcast_near(cpos->x, cpos->z, radius, player_id_,
                                 proto::play_cb::kEntityStatus, packet);
}

void Connection::spawn_dropped_item_at(double x, double y, double z, item::ItemStack stack) {
    const auto cpos = world::ChunkPos::from_world(static_cast<std::int32_t>(x),
                                                   static_cast<std::int32_t>(z));
    const auto [vx, vy, vz] = net::throw_velocity();
    drop_stack(x, y - 0.3 + 1.62, z, std::move(stack), cpos ? cpos->x : 0, cpos ? cpos->z : 0, vx,
               vy, vz, 40);
}

void Connection::kill_player() {
    if (dead_) {
        return;
    }
    dead_ = true;
    health_ = 0.0f;
    // UpdateHealth (0x41)：float health=0 | varint food | float saturation
    // 血量为 0 时客户端显示死亡界面，等待玩家点“重生”回 ClientCommand(0)
    cyane::ByteWriter health;
    health.f32(0.0f);
    health.varint(20);
    health.f32(0.0f);
    send_packet(proto::play_cb::kUpdateHealth, health.data());
    // EntityStatus 3 = 死亡动画，向他人广播
    if (context_.hub != nullptr) {
        cyane::ByteWriter status;
        writers::write_entity_status(status, player_id_, 3);
        context_.hub->broadcast(player_id_, proto::play_cb::kEntityStatus, status.data());
    }
    log::info("{} died", username_);
}

void Connection::respawn_player() {
    dead_ = false;
    health_ = 20.0f;
    player_pos_ = spawn_point();

    // Respawn (0x35)：int dimension | byte difficulty | byte gameMode | string levelType
    // 1.12.2 客户端同维度重生不重置世界；对齐 vanilla：不重发 JoinGame
    // （实测 Respawn 后再发 JoinGame 会强制客户端重建世界并卡在"加载地形"）
    cyane::ByteWriter respawn;
    respawn.i32(0);  // overworld
    respawn.u8(2);   // normal
    respawn.u8(context_.game_mode);
    respawn.string("default");
    send_packet(proto::play_cb::kRespawn, respawn.data());

    // vanilla 重生序列：Abilities → Health → SpawnPosition → TimeUpdate → HeldItem
    send_abilities_for(context_.game_mode);
    cyane::ByteWriter health;
    health.f32(20.0f);
    health.varint(20);
    health.f32(5.0f);
    send_packet(proto::play_cb::kUpdateHealth, health.data());
    cyane::ByteWriter spawn_pos;
    spawn_pos.position(context_.spawn_x, context_.spawn_y, context_.spawn_z);
    send_packet(proto::play_cb::kSpawnPosition, spawn_pos.data());
    cyane::ByteWriter time;
    time.i64(0);
    time.i64(0);
    send_packet(proto::play_cb::kTimeUpdate, time.data());
    cyane::ByteWriter held;
    writers::write_held_item_change(held, selected_slot_);
    send_packet(proto::play_cb::kHeldItemChange, held.data());

    // 出生点区块可能已被客户端按 UnloadChunk 丢弃：清表重发（重复 ChunkData 就地覆盖）
    loaded_chunks_.clear();
    pending_chunks_.clear();
    pending_chunk_keys_.clear();
    has_center_ = false;
    const auto spawn_chunk = world::ChunkPos::from_world(
        static_cast<std::int32_t>(player_pos_.x), static_cast<std::int32_t>(player_pos_.z));
    update_view(spawn_chunk.value_or(world::ChunkPos{0, 0}));

    // 同维度重生客户端不清世界：不补发实体（避免双份）；
    // 他人端销毁旧实体后按新位置重发本玩家的 SpawnPlayer。
    // broadcast_despawn 会把玩家从 hub 注销——必须重新注册，
    // 否则重生后收不到任何广播（方块/实体/聊天），对他人也不可见。
    broadcast_despawn();
    register_in_hub();
    broadcast_spawn();

    // 客户端 NetHandlerPlayClient.handleRespawn → setDimensionAndSpawnPlayer 里
    // world.removeAllEntities() 且重建全新背包：物品栏必须重发；实体不做全量补发——
    // 与区块绑定（update_view 重流区块时 send_chunk_entities 按块补发，避免与
    // 全量补发叠加造成客户端同 id 重复 remove/add 的分身/抽搐）。
    send_inventory();

    ++teleport_id_;
    cyane::ByteWriter tp;
    tp.f64(player_pos_.x);
    tp.f64(player_pos_.y);
    tp.f64(player_pos_.z);
    tp.f32(player_pos_.yaw);
    tp.f32(player_pos_.pitch);
    tp.u8(0);
    tp.varint(teleport_id_);
    send_packet(proto::play_cb::kPlayerPositionLook, tp.data());

    // 向他人广播复活后的位置
    if (context_.hub != nullptr) {
        context_.hub->update_position(player_id_, player_pos_.x, player_pos_.y, player_pos_.z,
                                      player_pos_.yaw, player_pos_.pitch);
    }
    log::info("{} respawned", username_);
}

}
