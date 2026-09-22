#include <cstdint>
#include <string>

#include "cyane/core/bytes.hpp"
#include "test_framework.hpp"

CYANE_TEST(bytes_varint_round_trips_every_width) {
    constexpr std::int32_t kSamples[] = {0, 1, 2, 127, 128, 255, 256, 2097151, 2147483647, -1, -2147483648};
    for (const std::int32_t value : kSamples) {
        cyane::ByteWriter writer;
        writer.varint(value);
        cyane::ByteReader reader{writer.data()};
        const auto decoded = reader.varint();
        CYANE_CHECK(decoded.has_value());
        CYANE_CHECK_EQ(decoded.value_or(0), value);
        CYANE_CHECK(reader.empty());
    }
}

CYANE_TEST(bytes_varint_matches_wire_length) {
    struct Sample {
        std::int32_t value;
        std::size_t bytes;
    };
    constexpr Sample kSamples[] = {
        {0, 1}, {127, 1}, {128, 2}, {16383, 2}, {16384, 3}, {2097151, 3}, {2097152, 4}, {2147483647, 5}, {-1, 5}};
    for (const auto& sample : kSamples) {
        cyane::ByteWriter writer;
        writer.varint(sample.value);
        CYANE_CHECK_EQ(writer.size(), sample.bytes);
    }
}

CYANE_TEST(bytes_varint_encodes_known_vectors) {
    cyane::ByteWriter writer;
    writer.varint(300);
    CYANE_CHECK_EQ(writer.size(), std::size_t{2});
    CYANE_CHECK_EQ(std::to_integer<std::uint8_t>(writer.data()[0]), std::uint8_t{0xAC});
    CYANE_CHECK_EQ(std::to_integer<std::uint8_t>(writer.data()[1]), std::uint8_t{0x02});
}

CYANE_TEST(bytes_varint_rejects_truncated_and_overlong) {
    const cyane::Bytes truncated{std::byte{0x80}, std::byte{0x80}};
    cyane::ByteReader reader{truncated};
    CYANE_CHECK(!reader.varint().has_value());

    const cyane::Bytes overlong{
        std::byte{0x80}, std::byte{0x80}, std::byte{0x80}, std::byte{0x80}, std::byte{0x80}, std::byte{0x01}};
    cyane::ByteReader long_reader{overlong};
    const auto decoded = long_reader.varint();
    CYANE_CHECK(!decoded.has_value());
    CYANE_CHECK(decoded.error().code == cyane::ErrorCode::protocol);
}

CYANE_TEST(bytes_varlong_round_trips) {
    constexpr std::int64_t kSamples[] = {0, 1, 9223372036854775807LL, -1, -9223372036854775807LL - 1};
    for (const std::int64_t value : kSamples) {
        cyane::ByteWriter writer;
        writer.varlong(value);
        cyane::ByteReader reader{writer.data()};
        CYANE_CHECK_EQ(reader.varlong().value_or(0), value);
    }
}

CYANE_TEST(bytes_big_endian_round_trips) {
    cyane::ByteWriter writer;
    writer.u16(0xBEEF);
    writer.i32(-123456789);
    writer.i64(0x0123456789ABCDEFLL);
    writer.f64(3.5);

    CYANE_CHECK_EQ(std::to_integer<std::uint8_t>(writer.data()[0]), std::uint8_t{0xBE});
    CYANE_CHECK_EQ(std::to_integer<std::uint8_t>(writer.data()[1]), std::uint8_t{0xEF});

    cyane::ByteReader reader{writer.data()};
    CYANE_CHECK_EQ(reader.u16().value_or(0), std::uint16_t{0xBEEF});
    CYANE_CHECK_EQ(reader.i32().value_or(0), -123456789);
    CYANE_CHECK_EQ(reader.i64().value_or(0), 0x0123456789ABCDEFLL);
    CYANE_CHECK_NEAR(reader.f64().value_or(0.0), 3.5, 1e-12);
    CYANE_CHECK(reader.empty());
}

CYANE_TEST(bytes_string_round_trips_utf8) {
    cyane::ByteWriter writer;
    writer.string("Cyane§bServer");
    writer.string("中文测试");
    writer.string("");

    cyane::ByteReader reader{writer.data()};
    CYANE_CHECK_EQ(reader.string().value_or(""), std::string{"Cyane§bServer"});
    CYANE_CHECK_EQ(reader.string().value_or(""), std::string{"中文测试"});
    CYANE_CHECK_EQ(reader.string().value_or("x"), std::string{});
    CYANE_CHECK(reader.empty());
}

CYANE_TEST(bytes_string_length_prefix_counts_bytes_not_chars) {
    cyane::ByteWriter writer;
    writer.string("中文");
    CYANE_CHECK_EQ(writer.size(), std::size_t{7});
    CYANE_CHECK_EQ(std::to_integer<std::uint8_t>(writer.data()[0]), std::uint8_t{6});
}

CYANE_TEST(bytes_rejects_invalid_utf8_and_oversized_strings) {
    cyane::ByteWriter writer;
    writer.varint(2);
    writer.u8(0xC3);
    writer.u8(0x28);
    cyane::ByteReader reader{writer.data()};
    const auto decoded = reader.string();
    CYANE_CHECK(!decoded.has_value());
    CYANE_CHECK(decoded.error().code == cyane::ErrorCode::protocol);

    cyane::ByteWriter huge;
    huge.varint(40000);
    cyane::ByteReader huge_reader{huge.data()};
    CYANE_CHECK(!huge_reader.string().has_value());
}

CYANE_TEST(bytes_accepts_valid_multibyte_utf8) {
    CYANE_CHECK(cyane::is_valid_utf8("ascii"));
    CYANE_CHECK(cyane::is_valid_utf8("中文"));
    CYANE_CHECK(cyane::is_valid_utf8("emoji \xF0\x9F\x98\x80"));
    CYANE_CHECK(cyane::is_valid_utf8("\xC2\xA7"
                                     "a"));
    CYANE_CHECK(!cyane::is_valid_utf8("\xC0\x80"));
    CYANE_CHECK(!cyane::is_valid_utf8("\xE0\x80\x80"));
    CYANE_CHECK(!cyane::is_valid_utf8("\xED\xA0\x80"));
    CYANE_CHECK(!cyane::is_valid_utf8("\xF5\x80\x80\x80"));
    CYANE_CHECK(!cyane::is_valid_utf8("\xC3"));
}

CYANE_TEST(bytes_position_layout_matches_protocol_340) {
    // 由 BlockPosition.asLong 推出：x=1,y=2,z=3 → 2^38 + 2^27 + 3
    CYANE_CHECK_EQ(static_cast<std::uint64_t>(cyane::encode_position(1, 2, 3)), std::uint64_t{0x4008000003});
    CYANE_CHECK_EQ(cyane::position_y(cyane::encode_position(1, 2047, 2)), std::int32_t{2047});
}

CYANE_TEST(bytes_position_round_trips_including_negatives) {
    struct Sample {
        std::int32_t x;
        std::int32_t y;
        std::int32_t z;
    };
    constexpr Sample kSamples[] = {
        {0, 0, 0}, {1, 2, 3}, {-1, -1, -1}, {1000000, 255, -1000000}, {-30000000, -100, 30000000}, {1, 2047, -2048}};
    for (const auto& sample : kSamples) {
        const std::int64_t packed = cyane::encode_position(sample.x, sample.y, sample.z);
        CYANE_CHECK_EQ(cyane::position_x(packed), sample.x);
        CYANE_CHECK_EQ(cyane::position_y(packed), sample.y);
        CYANE_CHECK_EQ(cyane::position_z(packed), sample.z);
    }
}

CYANE_TEST(bytes_reader_tracks_consumption) {
    const cyane::Bytes data{std::byte{0x01}, std::byte{0x02}, std::byte{0x03}, std::byte{0x04}};
    cyane::ByteReader reader{data};
    CYANE_CHECK_EQ(reader.remaining(), std::size_t{4});
    CYANE_CHECK_EQ(reader.u8().value_or(0), std::uint8_t{1});
    CYANE_CHECK_EQ(reader.offset(), std::size_t{1});
    CYANE_CHECK_EQ(reader.remaining(), std::size_t{3});
    CYANE_CHECK_EQ(reader.big_endian<std::uint16_t>().value_or(0), std::uint16_t{0x0203});
    CYANE_CHECK_EQ(reader.remaining(), std::size_t{1});
    CYANE_CHECK(!reader.big_endian<std::uint32_t>().has_value());
    CYANE_CHECK_EQ(reader.remaining(), std::size_t{1});
}
