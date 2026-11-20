#include <cstring>
#include <string_view>

#include "cyane/item/item_stack.hpp"
#include "cyane/item/item_tools.hpp"
#include "cyane/net/combat_rules.hpp"
#include "cyane/net/packet_writers.hpp"
#include "cyane/proto/play_fields.hpp"
#include "cyane/world/blocks.hpp"
#include "test_framework.hpp"

using namespace cyane;

namespace {
// 断言 ByteWriter 载荷与十六进制串一致（每字节两位，如 "01 80 01"）
[[nodiscard]] bool payload_is(const ByteWriter& out, std::string_view hex) {
    const auto& data = out.data();
    std::string actual;
    char buf[4];
    for (std::size_t i = 0; i < data.size(); ++i) {
        std::snprintf(buf, sizeof(buf), "%02x",
                      std::to_integer<unsigned>(data[i]));
        if (i != 0) {
            actual += ' ';
        }
        actual += buf;
    }
    if (actual != hex) {
        std::print(stderr, "    payload mismatch:\n      expected: {}\n      actual:   {}\n", hex,
                   actual);
        return false;
    }
    return true;
}
} // namespace

// 黄金向量来源：wiki.vg 1.12.2 协议事实 + 真实客户端断连事故复盘（R-016/R-017）。
// 实体 id 一律 varint（EntityStatus 除外）。修改编码必须同步更新本测试。

CYANE_TEST(packet_animation_uses_varint_entity_id) {
    ByteWriter out;
    net::writers::write_animation(out, 1, 0);
    CYANE_CHECK(payload_is(out, "01 00"));
    // entityId ≥128 走两字节 varint——曾经的 i32 bug 会让这里变成 4 字节
    ByteWriter big;
    net::writers::write_animation(big, 128, 1);
    CYANE_CHECK(payload_is(big, "80 01 01"));
}

CYANE_TEST(packet_block_break_animation_layout) {
    ByteWriter out;
    net::writers::write_block_break_animation(out, 1, 10, 64, 10, 3);
    // varint(1) | position(10,64,10) | byte 3
    ByteWriter pos;
    pos.position(10, 64, 10);
    // 手动拼接：position 的 hex
    const auto& pd = pos.data();
    std::string pos_hex;
    char buf[4];
    for (std::size_t i = 0; i < pd.size(); ++i) {
        std::snprintf(buf, sizeof(buf), "%02x", std::to_integer<unsigned>(pd[i]));
        if (i != 0) {
            pos_hex += ' ';
        }
        pos_hex += buf;
    }
    CYANE_CHECK(payload_is(out, ("01 " + pos_hex + " 03")));
    CYANE_CHECK_EQ(out.data().size(), std::size_t{10});  // 1 + 8 + 1
}

CYANE_TEST(packet_held_item_change_is_single_byte) {
    ByteWriter out;
    net::writers::write_held_item_change(out, 2);
    CYANE_CHECK(payload_is(out, "02"));
    CYANE_CHECK_EQ(out.data().size(), std::size_t{1});
}

CYANE_TEST(packet_collect_item_three_varints) {
    ByteWriter out;
    net::writers::write_collect_item(out, 5, 1, 3);
    CYANE_CHECK(payload_is(out, "05 01 03"));
}

CYANE_TEST(packet_entity_status_uses_int_entity_id) {
    ByteWriter out;
    net::writers::write_entity_status(out, 7, 3);
    CYANE_CHECK(payload_is(out, "00 00 00 07 03"));  // int 大端
}

CYANE_TEST(packet_destroy_entities_count_plus_varints) {
    ByteWriter out;
    const std::uint32_t ids[] = {7, 128, 9};
    net::writers::write_destroy_entities(out, ids);
    CYANE_CHECK(payload_is(out, "03 07 80 01 09"));
}

CYANE_TEST(packet_entity_teleport_layout) {
    ByteWriter out;
    net::writers::write_entity_teleport(out, 3, 1.5, 64.0, 2.5, 90.0f, 0.0f, true);
    // varint(3) | 3×double | yaw(byte) | pitch(byte) | bool
    CYANE_CHECK_EQ(out.data().size(), std::size_t{1 + 24 + 2 + 1});
    // yaw=90° → 字节 0x40（256*90/360）
    const auto& d = out.data();
    CYANE_CHECK_EQ(std::to_integer<unsigned>(d[25]), 0x40u);
    CYANE_CHECK_EQ(std::to_integer<unsigned>(d[26]), 0x00u);
    CYANE_CHECK_EQ(std::to_integer<unsigned>(d[27]), 1u);
}

CYANE_TEST(packet_entity_head_look_layout) {
    ByteWriter out;
    net::writers::write_entity_head_look(out, 2, 180.0f);
    // varint(2) | byte(180°→0x80)
    CYANE_CHECK(payload_is(out, "02 80"));
}

// 权威参考：vanilla 1.12.2 RCON summon Item 后抓包——
// metadata 条目 = index byte + type varint + value；Item 实体 idx6 type5(Slot) + 6 字节 Slot + 0xFF
CYANE_TEST(packet_item_entity_metadata_matches_vanilla) {
    ByteWriter meta;
    meta.varint(0x0198);  // entityId 408（2 字节 varint）
    meta.u8(6);           // index：EntityItem 的物品堆叠
    meta.varint(5);       // type：Slot（1.9-1.12.2）
    item::write_slot(meta, item::ItemStack{3, 2, 0});  // dirt x2
    meta.u8(0xFF);
    CYANE_CHECK(payload_is(meta, "98 03 06 05 00 03 02 00 00 00 ff"));
}

CYANE_TEST(packet_angle_byte_wraps) {
    CYANE_CHECK_EQ(net::writers::angle_byte(0.0f), 0u);
    CYANE_CHECK_EQ(net::writers::angle_byte(90.0f), 64u);
    CYANE_CHECK_EQ(net::writers::angle_byte(180.0f), 128u);
    CYANE_CHECK_EQ(net::writers::angle_byte(270.0f), 192u);
    CYANE_CHECK_EQ(net::writers::angle_byte(360.0f), 0u);
}

// Named Sound Effect (0x49)：soundId 为 SoundEffect 注册表 id（VarInt，可多字节），位置是
// (方块坐标+0.5)×8 的定点数（负坐标按补码），类别为 SoundCategory 序数，音量/音高为 f32。
CYANE_TEST(packet_named_sound_encodes_registry_id_and_block_center) {
    ByteWriter out;
    net::writers::write_named_sound(out, 62, 4, 10, 64, -3, 0.3f, 0.6f);
    CYANE_CHECK(payload_is(out, "3e 04 00 00 00 54 00 00 02 04 ff ff ff ec 3e 99 99 9a 3f 19 99 9a"));

    ByteWriter big;
    net::writers::write_named_sound(big, 300, 4, 0, 0, 0, 1.0f, 1.0f);
    CYANE_CHECK(payload_is(big, "ac 02 04 00 00 00 04 00 00 00 04 00 00 00 04 3f 80 00 00 3f 80 00 00"));
}

CYANE_TEST(entity_shared_flags_bits_match_vanilla_setFlag) {
    // Entity.setFlag 的位序：0=着火、1=潜行、3=疾跑。
    // 曾经把疾跑写成 0x01，客户端会把疾跑玩家渲染成着火。
    using namespace cyane::proto::entity_flags;
    CYANE_CHECK_EQ(static_cast<int>(kOnFire), 0x01);
    CYANE_CHECK_EQ(static_cast<int>(kSneaking), 0x02);
    CYANE_CHECK_EQ(static_cast<int>(kSprinting), 0x08);
    CYANE_CHECK_EQ(static_cast<int>(kInvisible), 0x20);
    CYANE_CHECK_EQ(static_cast<int>(kGlowing), 0x40);
    // 疾跑与着火必须互不重叠
    CYANE_CHECK_EQ(static_cast<int>(kSprinting & kOnFire), 0);
    CYANE_CHECK_EQ(static_cast<int>(kSprinting & kSneaking), 0);
}

CYANE_TEST(game_mode_parses_names_shortnames_and_ids) {
    using namespace cyane::proto::game_mode;
    CYANE_CHECK_EQ(static_cast<int>(*parse("survival")), 0);
    CYANE_CHECK_EQ(static_cast<int>(*parse("s")), 0);
    CYANE_CHECK_EQ(static_cast<int>(*parse("c")), 1);
    CYANE_CHECK_EQ(static_cast<int>(*parse("a")), 2);
    CYANE_CHECK_EQ(static_cast<int>(*parse("sp")), 3);
    CYANE_CHECK(!parse("nope").has_value());
    for (std::int32_t id = 0; id <= 3; ++id) {
        CYANE_CHECK_EQ(static_cast<int>(*from_id(id)), id);
    }
    CYANE_CHECK(!from_id(-1).has_value());
    CYANE_CHECK(!from_id(4).has_value());
}

CYANE_TEST(item_equipment_slot_matches_getSlotForItemStack) {
    using cyane::item::equipment_slot;
    // EntityLiving.getSlotForItemStack：护甲按 armorType，鞘翅→胸甲，盾→副手，
    // 南瓜/头颅→头盔，其余无专用槽。槽位 = ContainerPlayer 的 8-index。
    CYANE_CHECK_EQ(equipment_slot(298), 5);  // 皮革头盔
    CYANE_CHECK_EQ(equipment_slot(311), 6);  // 钻石胸甲
    CYANE_CHECK_EQ(equipment_slot(300), 7);  // 皮革护腿
    CYANE_CHECK_EQ(equipment_slot(317), 8);  // 金靴子
    CYANE_CHECK_EQ(equipment_slot(443), 6);  // 鞘翅
    CYANE_CHECK_EQ(equipment_slot(442), 45); // 盾牌
    CYANE_CHECK_EQ(equipment_slot(86), 5);   // 南瓜
    CYANE_CHECK_EQ(equipment_slot(397), 5);  // 头颅
    CYANE_CHECK_EQ(equipment_slot(276), -1); // 钻石剑无专用槽
    CYANE_CHECK_EQ(equipment_slot(264), -1); // 钻石
}

CYANE_TEST(item_attack_speed_matches_tool_material_modifiers) {
    using cyane::item::attack_speed;
    // 基础 4.0；剑 -2.4；斧 ATTACK_SPEEDS{-3.2,-3.2,-3.1,-3.0,-3.0}
    for (const int sword : {268, 272, 267, 276, 283}) {
        CYANE_CHECK_EQ(attack_speed(static_cast<std::int16_t>(sword)), 1.6f);
    }
    CYANE_CHECK_EQ(attack_speed(271), 0.8f);  // 木斧
    CYANE_CHECK_EQ(attack_speed(275), 0.8f);  // 石斧
    CYANE_CHECK_EQ(attack_speed(258), 0.9f);  // 铁斧
    CYANE_CHECK_EQ(attack_speed(279), 1.0f);  // 钻石斧
    CYANE_CHECK_EQ(attack_speed(286), 1.0f);  // 金斧
    // 镐/锹/锄不加攻击速度修饰符
    CYANE_CHECK_EQ(attack_speed(257), 4.0f);  // 铁镐
    CYANE_CHECK_EQ(attack_speed(-1), 4.0f);   // 空手
}

CYANE_TEST(attack_cooldown_scaling_matches_vanilla) {
    // getCooledAttackStrength(0.5) = clamp((ticks + 0.5) / (20 / attackSpeed), 0, 1)
    // damage *= 0.2 + strength^2 * 0.8
    const auto scaled = [](float strength) { return 0.2f + strength * strength * 0.8f; };
    using cyane::item::attack_speed;
    const float sword_period = 20.0f / attack_speed(276);  // 12.5
    const auto strength_after = [&](int ticks) {
        return std::clamp((static_cast<float>(ticks) + 0.5f) / sword_period, 0.0f, 1.0f);
    };
    // 连点：0 tick 后只有 0.2 倍
    CYANE_CHECK(scaled(strength_after(0)) < 0.3f);
    // 挥满：13 tick 后达到 1.0（12.5 周期 + 0.5 偏移）
    CYANE_CHECK_EQ(strength_after(13), 1.0f);
    CYANE_CHECK_EQ(scaled(strength_after(13)), 1.0f);
    // 单调递增
    CYANE_CHECK(scaled(strength_after(6)) < scaled(strength_after(13)));
}

// 爆击 / 疾跑击退 / 横扫的判定真值表（EntityPlayer.attackTargetEntityWithCurrentItem）
CYANE_TEST(attack_outcome_matches_vanilla_rules) {
    using cyane::net::AttackConditions;
    using cyane::net::resolve_attack;

    {
        const auto r = resolve_attack(AttackConditions{.charged = true, .on_ground = true,
                                                       .holding_sword = true,
                                                       .walked_toward = true});
        CYANE_CHECK(r.sweeping);
        CYANE_CHECK(!r.crit);
        CYANE_CHECK_NEAR(r.knockback, 0.4f, 1e-6);
    }
    {
        const auto r = resolve_attack(AttackConditions{.charged = true, .on_ground = true,
                                                       .holding_sword = false,
                                                       .walked_toward = true});
        CYANE_CHECK(!r.sweeping);
    }
    {
        const auto r = resolve_attack(AttackConditions{.charged = true, .on_ground = true,
                                                       .holding_sword = true,
                                                       .walked_toward = false});
        CYANE_CHECK(!r.sweeping);
    }
    {
        const auto r = resolve_attack(AttackConditions{.charged = true,
                                                       .fall_distance = 1.5f,
                                                       .on_ground = false,
                                                       .holding_sword = true,
                                                       .walked_toward = true});
        CYANE_CHECK(r.crit);
        CYANE_CHECK(!r.sweeping);
    }
    {
        const auto r = resolve_attack(AttackConditions{.charged = true,
                                                       .fall_distance = 1.5f,
                                                       .on_ground = false,
                                                       .sprinting = true,
                                                       .holding_sword = true,
                                                       .walked_toward = true});
        CYANE_CHECK(!r.crit);
        CYANE_CHECK(!r.sweeping);
        CYANE_CHECK(r.sprint_knockback);
        CYANE_CHECK_NEAR(r.knockback, 0.5f, 1e-6);
    }
    {
        const auto r = resolve_attack(AttackConditions{.charged = true,
                                                       .fall_distance = 1.5f,
                                                       .on_ground = false,
                                                       .in_water = true});
        CYANE_CHECK(!r.crit);
    }
    {
        const auto r = resolve_attack(AttackConditions{.charged = false,
                                                       .fall_distance = 1.5f,
                                                       .on_ground = false,
                                                       .holding_sword = true,
                                                       .walked_toward = true});
        CYANE_CHECK(!r.crit);
        CYANE_CHECK(!r.sweeping);
        CYANE_CHECK(!r.sprint_knockback);
    }
}
