#include <cstring>
#include <string_view>

#include "cyane/item/item_stack.hpp"
#include "cyane/net/packet_writers.hpp"
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

// Named Sound Effect (0x49)：位置是方块中心 ×8 的定点数（负坐标按补码），类别为 SoundCategory 序数，
// 音量/音高为 f32。
CYANE_TEST(packet_named_sound_encodes_block_center) {
    ByteWriter out;
    net::writers::write_named_sound(out, "block.lever.click", 4, 10, 64, -3, 0.3f, 0.6f);
    CYANE_CHECK(payload_is(out,
                           "11 62 6c 6f 63 6b 2e 6c 65 76 65 72 2e 63 6c 69 63 6b 04 00 00 00 58 "
                           "00 00 02 08 ff ff ff f0 3e 99 99 9a 3f 19 99 9a"));
}
